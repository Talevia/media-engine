## 2026-04-23 — thumbnail-impl (Milestone §M1 · Rubric §5.1)

**Context.** M1 exit criterion「`me_thumbnail_png` 实装」。`src/api/thumbnail.cpp` 一直是
纯 stub 返回 `ME_E_UNSUPPORTED`，`src/orchestrator/thumbnailer.cpp` 也是 stub；C API
调用压根不经过 orchestrator（签名不一致：C API 吃 URI，orchestrator 类吃 Timeline）。
probe-impl + reencode-h264-videotoolbox 两轮已经把 libavformat / libavcodec / libsws
依赖链都跑通了，thumbnail 是第三条——也是 M1 里 exit criteria 能一次闭环的最后
一个 asset 级 impl。

**Decision.**
- 直接在 `src/api/thumbnail.cpp` 里用 libav 管线实装，**绕过 orchestrator/Thumbnailer**。
  原因：orchestrator 那个类是 "timeline → thumbnail" 的设计，和 C API "uri → thumbnail"
  不对齐；本轮不改那个类（签名重构不在 thumbnail-impl 范围内）。PAIN_POINTS.md 有对应
  条目。
- 管线：strip `file://` → `avformat_open_input` → `avformat_find_stream_info` →
  `av_find_best_stream(VIDEO)` → 开解码器 → `avformat_seek_file(-1, target_us,
  AVSEEK_FLAG_BACKWARD)` 到目标 PTS 左侧的最近 key frame → `avcodec_flush_buffers`
  → 读包解码到第一个 `pts ≥ target` 的帧（记住 last-before 做 EOF fallback）→
  `sws_scale` 到 `AV_PIX_FMT_RGB24` + 目标尺寸 → libavcodec PNG encoder (内置，LGPL)
  → `std::malloc` 拷贝 PNG bytes 到 caller-owned buffer（和 `me_buffer_free` →
  `std::free` 配对）。
- `fit_bounds(native_w, native_h, max_w, max_h, out_w, out_h)` 为 M1 的
  "0 means native, otherwise fit in bounding box preserving aspect" 语义做了一次化简：
  两个 0 → 原生 passthrough；放大（r ≥ 1.0）不做（PNG 不是用来放大的）；只给一维上限
  另一维自由 → 另一维乘 r。
- Seek 失败的兜底：若 `target_us ≤ AV_TIME_BASE`（即要求的时间 ≤ 1s），seek 失败不当
  fatal 处理——某些容器（concat、HLS 片段、raw stream）不支持 seek，但 t=0 / t 很小
  时从头解码成本可接受。`target_us > 1s` 仍 seek 失败 → `ME_E_IO` + 诊断串。
- Rational-time 端对齐：time 来自 `me_rational_t`，内部转 `AV_TIME_BASE` 单位
  `av_rescale_q(t.num, {1, t.den}, AV_TIME_BASE_Q)`，再一次 `av_rescale_q(..., 
  vstream->time_base)` 得到 stream time_base 下的 PTS。`t.den ≤ 0` 做 clamp 到 1，
  `t.num < 0` clamp 到 0（public 参数守护；CLAUDE.md invariant 3 时间不浮点）。
- `std::bad_alloc` / `std::exception` catch-all 包住整个 `probe_and_render`，
  翻译成 `ME_E_OUT_OF_MEMORY` / `ME_E_INTERNAL`——C API 零异常逸出（CLAUDE.md
  invariant 1 / §3a.4）。
- `examples/06_thumbnail/` 作为 smoke + C-only 编译边界：`<uri> <num/den> <max-w>
  <max-h> <out.png>` CLI；`num/den` 解析避免命令行上出现 float（`1/1` = 1s，`15/10`
  = 1.5s），与 engine 内部 rational 约定对齐。

**Alternatives considered.**
- **直接用 libpng（或 spng / fpng）**：比 libavcodec 的 PNG encoder 小很多，压缩率更高。
  但 libpng 是新增 FetchContent/系统依赖，ARCHITECTURE.md 白名单里没有；spng 维护度
  不高；fpng 是 MIT 但没发 release tag。libavcodec 的 PNG encoder 已经在链图里，
  encoder name `png` 走 `AV_CODEC_ID_PNG`，零新依赖——M1 目标是 "把 libav 跑起来"，
  不是 "在 libav 基础上再自研 IO"。拒。
- **JPEG 替代 PNG**：比 PNG 小；但 API 明确命名为 `me_thumbnail_png`，签名不变（§3a.10
  ABI 稳定），JPEG 要加新函数 `me_thumbnail_jpeg`，现在不必。拒。
- **通过 `Thumbnailer` orchestrator 类走"伪 timeline"**（构造一个单 clip timeline 包
  URI，再调 png_at）：符合"一切走 orchestrator"的对称美，但给一次一帧的 asset-level
  操作套 Timeline → Graph → Schedule 的机器是架构倒挂。`Thumbnailer` 本质是 
  composition-级工具（未来 M3-M6 复合多轨后取帧），现在的 C API 是 asset-级工具，
  两者应是两条路，不是一条。拒——记 PAIN_POINTS，类以后拆。
- **Seek 到关键帧然后返回那一帧（不 decode 到目标 PTS）**：最便宜，但 PTS 粗糙（可能
  偏差几秒），对缩略图定位不可接受（VISION §5.1 "asset-level tools: thumb 必须 PTS
  精确到 frame"）。拒。
- **Seek 失败直接 `ME_E_IO`**：最严格，但对 t=0 的 thumbnail 过于敏感——某些
  container 的 "seek to 0" 就是 `AVERROR(EINVAL)`，但从头读 packet 完全 OK。兜底
  `target ≤ 1s` 的宽容既避免伪阴性又不至于在大偏移时掩盖真正的 IO 问题。接受。

**Coverage.**
- `./build/examples/06_thumbnail/06_thumbnail /tmp/input.mp4 1/1 320 180 /tmp/thumb.png`
  → 7178 bytes PNG, `file` 确认 `PNG image data, 320 x 180, 8-bit/color RGB,
  non-interlaced`。
- 同样 `/tmp/input.mp4 0/1 0 0 /tmp/thumb-native.png` → 1920×1080 native，47691 bytes
  PNG，`file` 输出合法。
- 不存在文件 `/tmp/nope.mp4` → `thumbnail: i/o error (avformat_open_input: No such
  file or directory)`，exit=1，`me_engine_last_error` 填充。
- `cmake --build build` + `ctest --test-dir build` → 3/3 passed（tests 没动，已有
  suite 不受影响）。
- `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON -DME_BUILD_TESTS=ON`
  干净重建通过，`-Wall -Wextra -Wpedantic -Werror` 全绿。
- `01_passthrough` 回归仍产合法 2s MP4。
- C-only 边界：`examples/06_thumbnail/main.c` 用 `me_thumbnail_png` + `me_buffer_free`
  + `me_rational_t` + `me_status_str`，纯 C 编译通过。

**License impact.** 无新 FetchContent。用到的 `AV_CODEC_ID_PNG` 是 libavcodec 内置
encoder（LGPL）；`FFMPEG::swscale` 在上一轮 reencode-h264-videotoolbox 已加入
白名单；其他依赖未变。

**Registration.** 本轮变动的注册点：
- `TaskKindId` / kernel registry: 未动。
- `CodecPool` / `FramePool` / resource factory: 未动（thumbnail 直接用局部 unique_ptr
  codec ctx，和 probe-impl 同构，pool 接入留给后续 `codec-pool-impl` 专轴）。
- Orchestrator factory: 未动（`Thumbnailer` 类保持 stub——签名和 C API 不对齐，拆分
  留给后续专题；PAIN_POINTS 已记）。
- 新导出的 C API 函数: 未新增（`me_thumbnail_png` + `me_buffer_free` 符号不变；只是
  前者从 `ME_E_UNSUPPORTED` stub 变成真实现，stub 净 -1）。
- CMake target / install export / FetchContent_Declare: 未动；新增 example target
  `06_thumbnail`（不进 install）。
- JSON schema: 未动。
- 新 example: `examples/06_thumbnail/`。
