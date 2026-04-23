## 2026-04-23 — multi-track-compose-sink-wire（scope-A：ComposeSink 类 + Exporter 路由）（Milestone §M2 · Rubric §5.1）

**Context.** 连续三轮 rotation 之后终于正面切这条 bullet。之前每一轮都判断："再切一块 scaffold 的 ROI 低"、"rotate 到另一个 P1"；结果另两个 P1 (cross-dissolve-kernel, audio-mix-kernel) 也是架构级工作，同样需要多 cycle。停不住了。本 cycle 明确给出"per-frame compose loop"一个专用 class 和 factory，把 Exporter 的多轨 gate 从 early-return 的 UNSUPPORTED 迁到 ComposeSink::process() 内部的 UNSUPPORTED——即**同步→异步**的行为迁移，是 next cycle 真正写 frame-loop 时的"home"。

Before-state grep evidence：

- `grep -rn 'ComposeSink\|compose_sink' src/` 返回空——无此 class。
- `src/orchestrator/exporter.cpp:83-87`（本 cycle 前）早-return `ME_E_UNSUPPORTED` "multi-track compose not yet implemented"——多轨在 me_render_start 阶段同步被拒。
- 已有 4 条 enabler：`src/compose/alpha_over.{hpp,cpp}`、`src/compose/active_clips.{hpp,cpp}`、`src/compose/frame_convert.{hpp,cpp}`——全部在前几 cycle 落地，但**没任何生产代码**消费它们。

**Decision.**

1. **`src/orchestrator/compose_sink.{hpp,cpp}` 新 class + factory**：
   - `class ComposeSink final : public OutputSink` 持 `const me::Timeline&` 引用 + `SinkCommon` + `vector<ClipTimeRange>` + `CodecPool*` + video/audio bitrate。
   - `process(demuxes, err)` 当前返回 `ME_E_UNSUPPORTED` + err message 明示 "per-frame compose loop not yet implemented — see multi-track-compose-frame-loop backlog item" + 列出 4 个就位的 prereq symbol 帮后续 cycle 定位要用什么。
   - `make_compose_sink(tl, spec, common, ranges, pool, err)` factory：
     - codec 验证：`video_codec == "h264" && audio_codec == "aac"`，否则 return nullptr + err "multi-track compose currently requires..."（passthrough compose 物理不可能——stream-copy 无法 composite；其他 codec 是 M3+ scope）。
     - `pool` 非 null 校验（compose 必走 reencode，必用 pool）。
     - `clip_ranges` 非空校验。
     - `tl.tracks.size() >= 2` 校验（defensive——单轨应该走普通 make_output_sink）。

2. **`src/orchestrator/exporter.cpp` 多轨 gate 路由**：
   - 旧 `if (tl_->tracks.size() > 1) return ME_E_UNSUPPORTED` 替换成 `const bool is_multi_track = tl_->tracks.size() > 1;`。
   - `make_output_sink` 调用点改成条件路由：`is_multi_track ? make_compose_sink(*tl_, ...) : make_output_sink(...)`。
   - ComposeSink factory 拒绝 codec 的场景仍然 synchronous ME_E_UNSUPPORTED（sink nullptr → exporter return ME_E_UNSUPPORTED）。
   - ComposeSink factory accept 但 process() stub 返回 UNSUPPORTED 的场景是 **asynchronous**——me_render_start 返回 ME_OK，worker thread 跑 process() 写入 err_msg，`me_render_wait` 最终返回 UNSUPPORTED。
   
   这是 observable 行为变化（同步→异步），更新对应测试。

3. **`src/CMakeLists.txt`** 追加 `orchestrator/compose_sink.cpp` 到 media_engine sources。

4. **`tests/test_timeline_schema.cpp`** 两个 test case 更新 + 新增：
   - **改写**: "multi-track timeline is rejected at the render layer by Exporter" → "multi-track + passthrough codec is rejected synchronously by compose factory"。用 passthrough codec；仍然 sync UNSUPPORTED from me_render_start；err 含 "multi-track compose currently requires"（新 err phrasing from make_compose_sink factory）。
   - **新增**: "multi-track + h264/aac codec is rejected asynchronously by ComposeSink stub"。用 h264/aac；me_render_start ME_OK；me_render_wait → ME_E_UNSUPPORTED；err 含 "per-frame compose loop not yet implemented"。**这条是 contract tripwire**：下一 cycle 实装 frame loop 后同一个 TEST_CASE 应该 fail（unless 我更新它），迫使我在实装时同步更新测试——regression-safe。

5. **-Werror `-Wunused-private-field` 坑**：ComposeSink 先存了 `tl_ / common_ / ranges_ / pool_ / video_bitrate_ / audio_bitrate_` 给下一 cycle 用，但本 cycle 的 stub process() 不读它们。`(void)member;` cast-to-void 是 C++ 标准消告警手段——每个 member 加一行，跑得过 `-Werror -Wunused-private-field`。下一 cycle 真用时 delete 这些 (void) lines。

6. **BACKLOG**：删 `multi-track-compose-sink-wire`，P1 末尾加窄版 `multi-track-compose-frame-loop`（just the per-frame compose loop impl 从 ComposeSink 的 stub 开始写起）。

**Scope 理由**。本 cycle 是"class 骨架 + 路由 + 测试"的 scaffold cycle，deliverable 有限但**真实**：

- 架构位置定了：未来 frame loop 写在 ComposeSink::process() 内；不会再产生"到底这块放哪"的设计讨论。
- Exporter 路由完成：`is_multi_track` 分支清晰；删掉了 "early return" 风格的 gate，改成 "factory + process" 模式。
- 行为契约转成 async：以 rendering 生命周期的正确方向（wait-based error reporting）暴露 stub，next cycle 实装时不用再改 control flow，只改 process() 实现。
- 测试 regression-safe：async-reject test 将在 frame loop 上线后**立刻 fail**，迫使 commit 同步更新测试。

**Alternatives considered.**

1. **一把写完 frame loop + 立刻端到端可用** —— 拒：3 次 rotation 证明在 cycle-scale 内做不到；scaffold 先行是诚实 slicing。
2. **把 compose 直接塞进 `make_output_sink`**（统一工厂）—— 拒：`make_output_sink` 不知道 Timeline（它只拿 clip_ranges flat 列表），补 Timeline 参数是 factory 接口变动，影响所有 sink branch。单独 `make_compose_sink` factory 隔离得更干净，现有 factory 签名不动。
3. **不迁移行为，保留 Exporter early-return** —— 拒：class 存在但 Exporter gate 还 synchronously 拒 = ComposeSink::process() 永远收不到调用，是死代码，下一 cycle 还要改 Exporter。一步到位。
4. **frame loop impl 用 `throw runtime_error`** —— 拒：CLAUDE.md invariant "C API is C, not C++"；exception 不能逸出 extern "C" 边界。返回 status code 是规范。
5. **给 ComposeSink 构造函数加 `_Wunused_` suppression attribute** 而非 `(void)` —— 拒：`[[maybe_unused]]` 是标记 declaration 的；member 用它得挂每个 member 前，噪声更大。cast-to-void 只加一段 6 行代码。
6. **把 codec 校验放 ComposeSink::process() 不放 factory** —— 拒：factory-time sync 拒绝对 host 是更好的 UX（立刻知道是 codec 不对还是 impl 待补）；process()-time async 适合 "待实装" 类 stub。两种通路分工合理。
7. **不加 `is_multi_track` 变量直接 inline `tl_->tracks.size() > 1`** —— 拒：read-local 变量化 express 了语义（multi-track is the decisive 分支条件）；inline 可能有人误读成 "两个独立 call sites"。

业界共识来源：Gstreamer 的 `GstBin` dispatch-to-subelement 模式、AVFoundation 的 `AVAssetExportSession` setup 同步 vs render 异步的错误分层、Ableton Live 的 "track playback engine" 把 track count 判断放 factory 的设计。本 cycle 的 sync-factory + async-process 分层与它们一致。

**Coverage.**

- `cmake --build build` + `-Werror` clean（第一次撞到 `-Wunused-private-field`，用 cast-to-void 解决）。
- `ctest --test-dir build` 20/20 suite 绿。
- `build/tests/test_timeline_schema` 47 case / 247 assertion（先前 46/?；-1 old + 2 new = +1 case net；新增 assertion +多，包括 async wait 路径的额外 checks）。
- 其他 19 suite 全绿。单轨 render 路径（passthrough / reencode）不受影响——`is_multi_track` 为 false 时走原 `make_output_sink` 路径。

**License impact.** 无新 dep。纯 C++ scaffolding。

**Registration.**
- `src/orchestrator/compose_sink.{hpp,cpp}` 新 class + factory。
- `src/CMakeLists.txt` 的 media_engine sources 追加 `orchestrator/compose_sink.cpp`。
- `src/orchestrator/exporter.cpp` 包含 compose_sink.hpp；多轨 gate 行为变迁。
- `tests/test_timeline_schema.cpp`：1 case 改写 + 1 case 新增。
- `docs/BACKLOG.md`：删 `multi-track-compose-sink-wire`，加 `multi-track-compose-frame-loop`。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加" 本 cycle **仍未完成**——ComposeSink 存在但 process() stub。§M.1 evidence check：`src/orchestrator/compose_sink.cpp:process` 的实装 body 还是 UNSUPPORTED 返回（不满足 "src impl 非 stub"条件）；该 exit criterion 保留未打勾。**下一 cycle 的 `multi-track-compose-frame-loop` 若闭环**，这条 criterion 会在那 cycle 的 §M.1 打勾。
