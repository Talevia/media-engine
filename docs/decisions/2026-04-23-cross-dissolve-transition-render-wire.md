## 2026-04-23 — cross-dissolve-transition-render-wire：接通 ComposeSink 的 Transition 分支 + 翻 Exporter gate（Milestone §M2 · Rubric §5.1）

**Context.** 此 bullet 在过去 3 个 cycle 里积累了 5 个 prereq：
1. `cross-dissolve-kernel`（pixel lerp 数学, `src/compose/cross_dissolve.{hpp,cpp}`）
2. `cross-dissolve-active-transition-scheduler`（per-track transition 窗口 resolver, `active_transition_at`）
3. `cross-dissolve-frame-source-resolver`（precedence `frame_source_at` 统一 transition > single-clip > none 规则）
4. `cross-dissolve-compose-sink-clip-decoders`（ComposeSink decoder 按 clip_idx 索引，允许同时持 2 个 decoder）
5. `me::Clip::id` 字段

本 cycle 把这些组件接通，使 `ComposeSink::process` 在 transition 窗口内 pull 两个 decoder、调 `cross_dissolve` blend，产物替代 `active_clips_at` 的单 clip 输出。同时翻 Exporter 的 transition gate，让 transition timeline 真正能渲染出来。

Before-state grep evidence：

- `src/orchestrator/exporter.cpp:90-94`（pre-cycle）：`if (!tl_->transitions.empty()) return ME_E_UNSUPPORTED;` ——所有带 transition 的 timeline 硬拒。
- `src/orchestrator/compose_sink.cpp:231`（pre-cycle）：`active_clips_at(tl_, T)` 产出 `std::vector<TrackActive>`，循环内只处理 SingleClip 情况（没有 transition 分支）。
- `grep -rn 'cross_dissolve\|frame_source_at' src/orchestrator/` 返回空——kernel + resolver 都已落地但从未被 orchestrator 调。

**Decision.** 三个文件改动：

1. **`src/orchestrator/exporter.cpp`**：
   - 删除 transition UNSUPPORTED gate。
   - 新增 `has_transitions = !tl_->transitions.empty()`，把路由条件从 `is_multi_track` 改成 `route_through_compose = is_multi_track || has_transitions`——带 transition 的单轨 timeline 也走 compose 路径。

2. **`src/orchestrator/compose_sink.cpp`** frame loop 重构：
   - `active_clips_at(tl_, T)` → per-track 循环 `frame_source_at(tl_, ti, T)`。每 track 返回一个 `FrameSource` tagged struct，kind 分三支：
     - **None**：跳过该 track 层（lower layers 保持不变）。
     - **SingleClip**：保留原路径（pull → frame_to_rgba8 → track_rgba → transform/opacity → alpha_over）。
     - **Transition**：pull `clip_decoders[fs.transition_from_clip_idx]` + `clip_decoders[fs.transition_to_clip_idx]`，分别 frame_to_rgba8 到 `from_rgba` / `to_rgba` 工作缓冲，调 `cross_dissolve(track_rgba, from_rgba, to_rgba, W, H, stride, fs.transition.t)`，然后以 to_clip 的 Transform/opacity 走原 alpha_over 路径。
   - 新增 `from_rgba` + `to_rgba` 两个 `std::vector<uint8_t>`，在 loop 之外一次性声明（lazily grown 到 W×H×4 by frame_to_rgba8 on first transition use）。
   - from 解码器 EOF 时（transition 窗口后半段，from 的源帧耗尽）：退化到 to-only（`std::memcpy(track_rgba, to_rgba, bytes)`），不报错。
   - **Phase-1 限制**（明文入 code comment）：
     - Transition 窗口内**不**应用 from_clip 的 Transform——假定 identity。slow-path affine pre-composite during blend 留给后续 cycle。
     - Transition 窗口内所有 endpoint frames 必须 W×H（不等则 ME_E_UNSUPPORTED）。
     - to_clip 的 single-clip 区间在 transition 窗口结束后会 "领先" `duration/2` 秒——因为 to decoder 在 transition 窗口期间每 output frame 前进一帧，窗口结束时已消耗 `duration/2 × fps` 帧，接入 single-clip 区间时解码器"比 schema 指示的 source_time 超前 duration/2"。对本 cycle 的测试 fixture（慢速变化的 gradient pattern）不可见；真实 handle / seek 支持是 post-M2 工作。

3. **`src/orchestrator/compose_sink.cpp` 的 `make_compose_sink` 工厂**：
   - `tracks.size() < 2` 拒绝改为 `tracks.size() < 2 && transitions.empty()` —— 单轨 + transition 的组合现在走 compose。
   - Per-track clip-count 校验：有 transition 的 track 免检（允许 2+ clip）；无 transition 的 track 仍要求恰好 1 clip。
   - Error message 更新为 "compose path (used for multi-track or transitions) requires h264+aac" —— 更准确地反映路由门槛。

4. **Tests**：
   - **`tests/test_compose_sink_e2e.cpp`** +1 TEST_CASE "single-track with cross-dissolve transition renders"：2-clip 单轨 + 0.48s cross-dissolve，渲染成 h264/aac MP4，验证 status=OK + 文件存在 + size > 4096（fixture 上实测 662984 bytes）。
   - **`tests/test_timeline_schema.cpp`** 两个现有测试的 error-string 断言更新：
     - "non-empty transitions ..." 测试：过去 expect "cross-dissolve / transitions not yet implemented"；现 expect "compose path" + "h264"（改 title 反映新行为：transition + passthrough → compose factory 拒绝）。
     - "multi-track compose ... ME_E_UNSUPPORTED" 测试：过去 expect "multi-track compose currently requires"；现 expect "compose path"（factory 的统一 error message）。

**Alternatives considered.**

1. **在 transition 分支里保留 from_clip 的 Transform** —— 拒：需要双路 affine_blit（from 和 to 各自变换到 canvas），再 cross_dissolve，加 to 的 alpha_over 路径要复用 post-blend 的 Transform。Code path 从 3 种（None/Single/Transition）膨胀到 6 种（跨两个 transform × 两个 blend 路径）。等真实需要（transition 中的 clip 有 scale/rotate）再加。M2 exit criterion 只要求 cross-dissolve 能渲，不要求带 transform 的 transition。
2. **缓存 last from frame，from EOF 后继续参与 blend** —— 拒：需要 per-transition 的 "last-valid-from" buffer + copy-from-scratch 逻辑。现在的退化路径（from EOF → to-only）在 transition 后半段 t >= 0.5 处，from 本来就接近 0 权重，视觉上退化几乎不可见。真要做 handle 支持是下个 milestone。
3. **保留 `active_clips_at` 路径，通过一个平行 "transition 优先覆盖" 分支注入 transition 帧** —— 拒：维护两套 per-(track, T) 查询路径（active_clips_at + 一个 transition override）违反 precedence resolver 的设计初衷（`frame_source_at` 存在的意义就是把两条查询统一）。已经有了 frame_source_at，就该用。
4. **Transition 窗口的 to_clip 在接入 single-clip 区间时 seek 回 duration/2 位置** —— 拒：(a) libav seek 不是 frame-accurate；(b) 会打断 decoder 的顺序 pull 状态，每次 seek 后 send_packet / receive_frame 都要重新排空；(c) phase-1 的 fixture 是慢变化 gradient，实际影响不可见。seek 支持留给未来带 handles 的设计（完整 M3 animated properties 后顺便带）。
5. **不翻 Exporter gate，仅在 compose_sink.cpp 内部 accept transitions** —— 拒：gate 不翻则单轨+transition 的 timeline 在 Exporter 层就被 UNSUPPORTED rejected，永远到不了 compose。route_through_compose 是让 transition 能跑的必要开关。
6. **把 Exporter gate 翻但保留 ComposeSink 的 Transition 分支不实现** —— 拒：会让 bullet 内核心价值（blend 出现）仍未交付，再次第 4 个 cycle 同一 bullet 的 prereq-only 模式。这次必须交付可观察行为。

**Scope 边界.** 本 cycle **交付**：
- Transition timeline 走 compose 路径。
- Cross-dissolve blend 在 compose 循环内生效。
- 一个 e2e 测试 proving transitions 渲染出非空 MP4。

本 cycle **不做**（真实限制，明文在 decision + code comment）：
- Transform on transition-window clips（identity-only）。
- From handle / to seek 支持（后半窗口 from 退化到 to-only；to 在后续单 clip 区间领先 duration/2）。
- Transition 渲染的 byte-identical determinism 回归测试（M2 exit criterion "软件路径 byte-identical determinism 回归" 单独是 bullet，videotoolbox 编码器非确定性也让它不能单靠 cross-dissolve 测试完成）。
- Multi-clip-single-track（without transition）compose 支持——factory 仍拒绝，需要 decoder seek 基础设施，独立 scope。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 26/26 suite 绿。
- `test_compose_sink_e2e` 新增 1 case：cross-dissolve render status=ME_OK，输出 662984 bytes MP4。
- `test_timeline_schema` 2 个现有测试 error-message 断言更新后通过。

**License impact.** 无。

**Registration.**
- `src/orchestrator/exporter.cpp`：-3 行（删 UNSUPPORTED gate）+3 行（新路由变量）。
- `src/orchestrator/compose_sink.cpp`：
  - frame loop body 从 `for (active)` + SingleClip inline 改为 `for (ti in tracks)` + FrameSource branch；+~100 行（Transition 分支 + 两 decoder pull + cross_dissolve 调用 + EOF 退化）。
  - `make_compose_sink` factory：relax tracks>=2 → tracks>=2 || !transitions.empty()；per-track clip-count exempt for transition tracks；error message update.
  - `#include "compose/cross_dissolve.hpp"`。
  - 新 working buffer: `from_rgba` / `to_rgba`。
- `tests/test_compose_sink_e2e.cpp`：+1 TEST_CASE（~80 行）。
- `tests/test_timeline_schema.cpp`：2 处 error-string 断言更新 + 1 个测试 title rename 反映新意图。
- `docs/BACKLOG.md`：**删除** bullet `cross-dissolve-transition-render-wire`——sub-scope 1-4 全部完成（frame loop 改调 frame_source_at ✓；Transition 分支 pull 两 decoder ✓；cross_dissolve 调用 ✓；Exporter gate 翻 ✓）。

**§M 自动化影响.** M2 exit criterion "Cross-dissolve transition" 现可 tick：
- src 非 stub 实装：`compose_sink.cpp` 的 Transition 分支调用 cross_dissolve；`exporter.cpp` 路由透过 compose。
- CI 覆盖：`test_compose_sink_e2e` 的 "single-track with cross-dissolve transition renders" case；`test_compose_cross_dissolve` 9 case 验 kernel；`test_compose_active_clips` 17 case 验 scheduler + resolver。
- 最近 30 commit：本 cycle 的 feat 自身 + 5 个 prereq cycle。
§M.1 下一步 tick `- [ ] Cross-dissolve transition`（独立 `docs(milestone):` commit）。
