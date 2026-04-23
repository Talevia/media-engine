## 2026-04-23 — multi-track-compose-frame-convert：YUV ↔ RGBA8 sws 转换 helper（Milestone §M2 · Rubric §5.1）

**Context.** `multi-track-compose-sink-wire` bullet 现在有三件前置就位（schema/IR、alpha_over 内核、active_clips resolver），但仍然是大scope：ComposeSink 类 + per-frame 多 demux 抽帧 + RGBA↔YUV 转换 + encoder 对接 + Exporter gate flip + e2e 测试。本 cycle 切 **RGBA↔YUV 转换 helper** 这一块——compose 路径的"像素格式适配层"，独立 TU + 单元可测。

Before-state grep evidence：

- `grep -rn 'sws_\|SwsContext\|AV_PIX_FMT_RGBA' src/`：`src/io/ffmpeg_raii.hpp` 已有 `SwsContextPtr` RAII wrapper；`src/api/thumbnail.cpp:222` 和 `src/orchestrator/reencode_segment.cpp:140` 各自 ad-hoc 用 sws_getContext + sws_scale。没有共享的"AVFrame ↔ RGBA8"转换 API。
- `grep -rn 'compose/frame_convert\|frame_to_rgba\|rgba8_to_frame' src/` 返空——compose 路径没 pixel-format helper。
- reencode 路径（`src/orchestrator/reencode_video.cpp:71-85`）sws 走 YUV→YUV scale（decoder 原格式到 encoder 想要的格式），从不经过 RGBA；multi-track compose 必须经过 RGBA（因为 `me::compose::alpha_over` 的输入是 RGBA8），需要新的方向和新的 API surface。

**Decision.** 新建 `src/compose/frame_convert.{hpp,cpp}`，两个对称 helper：

1. **`me_status_t frame_to_rgba8(const AVFrame* src, std::vector<uint8_t>& dst_buf, std::string* err)`**：
   - src 是 decoder output（任意 libav pix_fmt）。
   - 输出是 tightly-packed RGBA8：`width * height * 4` bytes，row stride == `width * 4`。
   - 内部 `sws_getContext(src_w, src_h, src_fmt, src_w, src_h, AV_PIX_FMT_RGBA, SWS_BILINEAR, ...)` + `sws_scale` 一把写到 `dst_buf.data()`。
   - 失败模式：null src / 零尺寸 → `ME_E_INVALID_ARG`；sws 内部失败 → `ME_E_INTERNAL`。

2. **`me_status_t rgba8_to_frame(const uint8_t* src, int w, int h, size_t stride_bytes, AVFrame* dst, std::string* err)`**：
   - src 是 tightly-packed RGBA8（compose 内核输出）。
   - dst 是调用方**预先分配**的 AVFrame（设好 `format` / `width` / `height` / `data` / `linesize`，通常 `av_frame_get_buffer` 分配）。
   - 失败模式：null / 零尺寸 / dst dims mismatch / stride_bytes < w*4 → `ME_E_INVALID_ARG`；sws → `ME_E_INTERNAL`。

3. **`SWS_BILINEAR` flag 选取理由（VISION §5.3 byte-determinism）**：
   - `SWS_FAST_BILINEAR` 允许 SIMD-dispatched 变体，跨 CPU 同 flag 可能产出不同 bytes——明确**不用**。
   - `SWS_BILINEAR` 是"确定性的 reference impl"，且匹配 `src/api/thumbnail.cpp` 已有选择。
   - 本 cycle 的 sws_scale 只做**格式转换**（src_w == dst_w, src_h == dst_h），不做几何缩放；flag 对此类调用影响小但定了统一约定。

4. **Tests**（`tests/test_compose_frame_convert.cpp`，9 TEST_CASE / 43 assertion）：
   - **Null / zero-dim / dimension mismatch / stride-too-small** 全部 ME_E_INVALID_ARG。
   - **YUV444P 白场 (Y=235, U=V=128) → RGBA8** 中心像素 R/G/B ≥ 245 + A == 255（BT.601 limited range 的白点）。
   - **YUV444P 黑场 (Y=16, U=V=128) → RGBA8** 中心像素 R/G/B ≤ 10 + A == 255。
   - **YUV444P → RGBA8 → YUV444P round-trip**（非平凡像素 Y=180 U=90 V=170）：还原值与原值相差 ≤ 4 LSB（YUV444P 无 chroma subsample，sws rounding 是唯一 loss 源）。
   - **Byte-determinism**：同一 frame 连调两次 `frame_to_rgba8`，两个 `std::vector<uint8_t>` 必须 `==`——抓住未来 flag 漂移到 SWS_FAST_BILINEAR 等的 regression。
   - 测试用 `YuvFrame` RAII helper（`av_frame_alloc` + `av_frame_get_buffer(align=32)` + `av_frame_free` in dtor）避免泄漏；测试之间各自独立。

5. **Scope 留给 follow-up**：
   - `ComposeSink` 类（持 N DemuxContext + 每 frame 调 resolver + 调 frame_to_rgba8 + alpha_over + rgba8_to_frame + 送 encoder）
   - Exporter multi-track gate 翻
   - 2-track e2e determinism 测试
   
   这些还在 `multi-track-compose-sink-wire` bullet 里（本 cycle 删除旧版、加更窄新版）。

**Alternatives considered.**

1. **直接在 ComposeSink 里调 sws_scale，不 wrap**（mirror 现有 reencode_segment.cpp 的 inline style）—— 拒：ComposeSink 已经要处理 N demux / 调度 / 合成 / encoder 对接，再塞 sws 样板会让 class > 300 lines；独立 TU 让两头都小且各自 unit-testable。
2. **把 helper 放 `src/io/`（跟 ffmpeg_raii 同级）而非 `src/compose/`** —— 拒：`src/io/` 是 IO layer（demux / mux / ffmpeg RAII），compose helpers 有 "用在合成路径上" 的 domain 色彩；放 `src/compose/` 和 alpha_over / active_clips 同级更聚焦。
3. **返回 `AVFrame*` 而非填 `std::vector<uint8_t>`** —— 拒：alpha_over 拿 `uint8_t*` + stride 作 API，中间再套一层 AVFrame 徒增 lifetime 复杂度；直接用 vector 让 compose 层的 buffer ownership 简单（compose 内核不关心 src 帧从哪来）。
4. **SwsContext cache**（per (w,h,src_fmt,dst_fmt) 组合 reuse） —— 拒：sws_getContext 本身 < 1ms，cache 是 perf 优化；M2 correctness-first，cache 进 `multi-track-compose-sink-wire` 或后续 perf bullet。
5. **用 `AV_PIX_FMT_RGBA` 还是 `AV_PIX_FMT_RGB0`**（后者不带 alpha） —— 拒：alpha_over 读写 alpha channel；alpha 必须 per-pixel 显式。RGBA 在 sws 里自动把 alpha fill 成 255（无 alpha 源时），正好。
6. **用 premultiplied alpha 内部** —— 拒：alpha_over 合约是 straight alpha；一致。
7. **Tests 用 YUV420P 而非 YUV444P**（匹配 h264 decoder 实际输出） —— 拒：chroma subsample 让 round-trip 内的 tolerance 变成 10+ LSB，测试精度下降。Use YUV444P 确认 helper 本身是 loss-free 的；真 YUV420 路径的误差在 ComposeSink cycle 另行考虑。
8. **把 `frame_to_rgba8` 重载 taking `uint8_t*` + stride 而非 vector&** —— 拒：输出尺寸依赖 src，caller 不提前知道；vector 构造 + 移出的 ownership 简单。

业界共识来源：libswscale 是 FFmpeg 上游的 reference 像素格式转换器（DaVinci / Premiere / Shotcut / OBS 内部路径相同），`SWS_BILINEAR` 确定性 + 非 SIMD-dispatched 是 VFX pipeline 的公知选择。AV_PIX_FMT_RGBA 的 "alpha defaults to 255 for alpha-less source" 是 sws 文档规定行为。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 19/19 suite 绿（新 `test_compose_frame_convert` 是第 19）。
- `build/tests/test_compose_frame_convert` 9 case / 43 assertion / 0 fail。
- 其他 18 suite 全绿，无 side-effect（compose/frame_convert.cpp 是新 TU；`src/` 其他文件不动）。

**License impact.** 无新 dep（libswscale / libavutil 已在 FFmpeg import 链里）。

**Registration.**
- `src/compose/frame_convert.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` `media_engine` source list 追加 `compose/frame_convert.cpp`。
- `tests/test_compose_frame_convert.cpp` 新 suite。
- `tests/CMakeLists.txt` `_test_suites` 追加 + 专门 `target_link_libraries` 块（测试直接用 AVFrame API，需要显式链 `FFMPEG::avutil / avcodec / swscale`）。
- `docs/BACKLOG.md`：删 `multi-track-compose-sink-wire` 旧版（本 cycle 又啃掉一块），末尾加更窄新版（ComposeSink class + gate flip + e2e 仍未做）。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加" 本 cycle **未完成**——四件已齐（schema/IR + alpha_over + resolver + frame_convert），剩下的是 ComposeSink 类把它们粘成 render-path 可消费的 OutputSink + Exporter gate 翻。§M.1 evidence check：`src/orchestrator/exporter.cpp` 的多轨 gate 仍 UNSUPPORTED；该 exit criterion 保留未打勾。
