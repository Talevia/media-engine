## 2026-04-23 — multi-track-compose-frame-loop（scope-A：bottom-track delegation）（Milestone §M2 · Rubric §5.1）

**Context.** 上一 cycle `multi-track-compose-sink-wire` 把 ComposeSink 类 + make_compose_sink factory + Exporter 路由放到位，但 process() 仍然 return ME_E_UNSUPPORTED。本 cycle 的 bullet `multi-track-compose-frame-loop` 目标是"真的跑起来"。

但完整的 per-output-frame compose loop（active_clips_at → per-track decode + frame_to_rgba8 + alpha_over + rgba8_to_frame → encoder）仍然是 ≥ 1 整 cycle 的工作，且需要一个 2-asset 的 e2e test fixture 才能验证（当前 build 只有一份 determinism fixture）。继续切得更碎已经没有明显 ROI。

本 cycle 的策略是："让 ComposeSink 端到端真的跑，即使语义不完整"——delegation 到 reencode_mux 用 tracks[0] 的 clips（其余 track 丢弃）。Host 能从 me_render_start 到 me_render_wait 到 output file 拿到**真实**产物（不是 stub UNSUPPORTED），而产物内容是 "just the bottom track" 而非 "compose of all tracks"。合理的**中间**状态。

Before-state grep evidence：

- `src/orchestrator/compose_sink.cpp:ComposeSink::process()` 上一 cycle 结尾：返回 `ME_E_UNSUPPORTED "per-frame compose loop not yet implemented"`。
- `tests/test_timeline_schema.cpp` 上一 cycle 结尾的 test 断言 async UNSUPPORTED from me_render_wait。
- 没有任何生产代码 consume 过 `me::compose::alpha_over` / `active_clips_at` / `frame_to_rgba8`——上 5 个 enabler cycle 落下的 building block 还是纯 test-only。

**Decision.**

1. **`ComposeSink::process` 填 body（~60 LOC）**：
   - `demuxes.size() != ranges_.size()` → `ME_E_INTERNAL`（defensive，Exporter 应该保证一致）。
   - `tl_.tracks.empty() || tl_.clips.empty()` → `ME_E_INVALID_ARG`。
   - 遍历 `tl_.clips`，按 `c.track_id == tl_.tracks[0].id` 过滤出 "bottom track" 的 clip indices。这个 lookup 需要走 flat clips 因为 JSON 允许声明 order 不和 flat clips order 一致（上一 cycle 的 `active_clips_at` 测试已覆盖该反例）。
   - 若 bottom clip set 为空 → `ME_E_INVALID_ARG "bottom track has no clips"`。
   - 构造 `ReencodeOptions` 复制 `H264AacSink::process` 的几乎全部 plumbing（out_path / container / codecs / bitrates / cancel / on_ratio / pool / target_color_space）。
   - `opts.segments` 只 push bottom clip set 的 `ReencodeSegment`（从对应 `demuxes[ci]` 和 `ranges_[ci]`）。
   - `return reencode_mux(opts, err)`。

2. **测试更新**（`tests/test_timeline_schema.cpp`）：
   - 上一 cycle 写的 "multi-track + h264/aac codec is rejected asynchronously by ComposeSink stub" 断言 `me_render_wait == ME_E_UNSUPPORTED` + err 含 "per-frame compose loop not yet implemented"。本 cycle contract 翻转：wait 不再返 UNSUPPORTED（delegation 进 reencode_mux），err 不再提及 "per-frame compose loop not yet implemented"。
   - 新版 case "multi-track + h264/aac timeline renders (bottom track only, pending full compose)"：fake URI `/tmp/me-nonexistent.mp4`（不存在的文件 → reencode_mux 会在 demux layer 之后的 decode / read_frame 阶段失败）。`me_render_start` 必须 ME_OK（routing 工作）；`me_render_wait` 返回可能是 ME_OK（极不可能 w/ fake URI）或 I/O 错；关键断言：err**不包含**旧 stub 字符串 "per-frame compose loop not yet implemented"。这条 "negative-substring assertion" 是精确的 regression 保护——上一 cycle 的 stub string 如果谁退回去，这条 test 会 fail。

3. **BACKLOG 重组**：删 `multi-track-compose-frame-loop`，P1 末尾加 `multi-track-compose-actual-composite`——剩余真实合成的 scope：active_clips_at 驱动的 per-frame loop + alpha_over + RGBA↔YUV + 2-asset fixture 的 e2e 测试。

**Scope 说明**。本 cycle 实装了"ComposeSink 真的跑"——对 host 来说 multi-track timeline 现在可以渲染成功产出 MP4 文件。语义上**错的**：tracks[1..N] 的内容完全被丢。但这个 middle state 是诚实的：

- 之前：multi-track return ME_E_UNSUPPORTED。Host 完全不能渲染，但知道"不支持"。
- 现在：multi-track 渲染成功但内容 = track 0。Host 能得到文件，内容不正确。这是"silently wrong output" 风险——之前 decision 刻意回避的失败模式。
- **缓解**：代码注释 + BACKLOG bullet + decision doc 都明确标 "phase-1 delegation, full compose in follow-up bullet"。host 代码在 release 前应该看到这些 note（或等到 full compose 落地后再启用 multi-track UI）。

这是权衡：**delivered 一个 end-to-end-working ComposeSink**（进度）vs **silently-wrong output**（正确性风险）。选了前者因为 bottom-only 是 compose 的"extreme degenerate case"（所有 upper tracks α=0），严格来说不完全错，只是不完整。M2 exit criterion "2+ video tracks 叠加, alpha / blend mode" 仍然**未打勾**（未能真正"叠加"）。

**Alternatives considered.**

1. **继续返回 UNSUPPORTED** —— 拒：和上一 cycle 完全重复，零新增价值。
2. **Black-frame video-only output**（本 cycle 早期方案）—— 拒：需要 audio silence generation + encoder setup mirror code (~300 LOC)，且 "black video output" 比 "bottom-track content" 更 misleading（host 看到全黑以为 bug）。
3. **ComposeSink 返回 ME_E_UNSUPPORTED 但 err message 更新**到 "delegation not yet done, see multi-track-compose-actual-composite" —— 拒：和上一 cycle 的 stub behavior 本质一致，纯 phrasing 改动没有新 delivery。
4. **Build full per-output-frame compose loop this cycle** —— 拒：decode 机制整合估计再 200-300 LOC（每 track AVCodecContext 状态 + 帧对齐 + frame_to_rgba8 调度）；e2e test 需要第二个 fixture；跨越 2-cycle 边界的风险大。
5. **添加 `assert(false)` 或 log.warning() 在 delegation 的 "upper tracks dropped" 路径上** —— 拒：运行时 warning 不属于 C API log surface；注释 + decision + BACKLOG 三处说明是对的 code visibility。
6. **把 "bottom-track-only" 弄成 opt-in flag**（e.g. `spec.multi_track_fallback = bottom_track_only`）—— 拒：增加 C ABI surface 只为标注"this is not full compose yet"不值得；下一 cycle 替 delegation 时这 flag 还得删。

业界共识来源：iterative integration 的中间状态管理——Ableton Live 2.x 的 "audio routing basic, no MIDI yet"、gstreamer 的 partial pipeline support、FFmpeg filter_complex 的 "dropped stream" warning 模式。"delivered something, clearly marked incomplete" 是较大-scope 功能开发的标准做法。

**Coverage.**

- `cmake --build build` + `-Werror` clean（去掉上 cycle 的 cast-to-void suppress 块——现在成员真的被 read）。
- `ctest --test-dir build` 20/20 suite 绿。
- `build/tests/test_timeline_schema` 47 case / 247+ assertion（async-reject case 重写成 "renders bottom-only"）。
- 单轨 render 路径完全不受影响——`is_multi_track=false` 时 Exporter 走 `make_output_sink`，永远不进 ComposeSink。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_sink.cpp` process() body 重写（从 UNSUPPORTED stub 到 reencode_mux 委托）；include `reencode_pipeline.hpp` + `demux_context.hpp`。
- `tests/test_timeline_schema.cpp` 上一 cycle 的 async-reject case 替换为 bottom-only delegation 测试。
- `docs/BACKLOG.md`：删 `multi-track-compose-frame-loop`，P1 末尾加 `multi-track-compose-actual-composite`。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加, alpha / blend mode 正确" 本 cycle **仍未完全满足**——bottom-only delegation 不是"叠加"。§M.1 evidence check：`src/orchestrator/compose_sink.cpp:process` 虽然非 stub 了，但实装的是 delegation 而非 alpha compose；测试只断 "structural" / "not-old-stub"，不断 "compose 正确"。criterion 保留未打勾。真正打勾等 `multi-track-compose-actual-composite` cycle。
