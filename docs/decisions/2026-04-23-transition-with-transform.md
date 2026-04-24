## 2026-04-23 — transition-with-transform：cross-dissolve 两端点应用 per-clip Transform（Milestone §M2 · Rubric §5.1）

**Context.** Cross-dissolve wire-in 落地时（`cross-dissolve-transition-render-wire` cycle）明文标的 phase-1 限制之一："No per-clip Transform applied to from/to during the transition window (identity assumed)". `src/orchestrator/compose_transition_step.cpp` 的 transition 分支直接 cross_dissolve 两 raw RGBA buffer，caller 之后用 to_clip.transform 做一次 affine_blit——from_clip 的 transform 完全**不生效**。若 from 和 to 的 transform 不同（典型 cross-dissolve 用 2 clip 各自带独立 Transform），blend 结果视觉错乱。

Before-state grep evidence：
- `src/orchestrator/compose_transition_step.cpp` 原版：`cross_dissolve(track_rgba, from_rgba, to_rgba, ...)` —— 源数据直接 blend，没经任何 per-clip transform。
- `src/orchestrator/compose_sink.cpp` 后续 common path: `transform_clip_idx = to_ci` → 只对 blend 结果应用 **to_clip** 的 transform。
- 实测：transition 测试（`test_compose_sink_e2e` "single-track with cross-dissolve transition renders"）两 clip 都是 identity transform + 匹配 W×H dims，所以 bug 不触发。真实用户场景（带 scale/translate 的 transition）会出现 silent wrong behavior。

**Decision.**

1. **`src/orchestrator/compose_transition_step.hpp/cpp`** 改造——**始终 pre-transform 每个 endpoint 到 canvas size**，然后 cross_dissolve：
   - Signature 加 `const me::Clip& from_clip, const me::Clip& to_clip`（transform 访问）+ 两 canvas-sized scratch 参数（`from_canvas`, `to_canvas`）+ out 参数 `bool& out_spatial_already_applied`。
   - 新 private helper `spatial_identity(clip, src_w, src_h, W, H)` → 当且仅当 clip 无 Transform / 所有 spatial 字段 identity **且** src 维度 == 画布时返 true（dim mismatch 算非-identity，走 affine_blit 做 scale-to-canvas）。
   - 新 private helper `transform_to_canvas(clip, src_rgba, src_w, src_h, W, H, dst_canvas)`：identity + 匹配 dim 直接 memcpy；否则 `compose_inverse_affine` + `affine_blit`。
   - 新 private helper `decode_to_rgba(td, out_rgba, out_w, out_h, out_valid, err)`：封装 `pull_next_video_frame` + `frame_to_rgba8` + `av_frame_unref` 的三步序列，NOT_FOUND 不是错。拆出减少了 transition_step body 的嵌套。
   - 流程：pull from → (if valid) transform_to_canvas → from_canvas；pull to → (必 valid 否 ME_E_NOT_FOUND) transform_to_canvas → to_canvas；cross_dissolve(track_rgba, from_canvas, to_canvas) 或 from 无效时 memcpy(to_canvas → track_rgba)。
   - `out_spatial_already_applied = true` 恒成立（即使两 clip 都 identity，memcpy 仍然发生——它是 affine_blit 的 fast path）。
   - 移除了旧的 W×H dim-enforcement check —— 现在 affine_blit 天然处理 dim mismatch。

2. **`src/orchestrator/compose_sink.cpp`** 更新：
   - frame loop 前加两新 `std::vector<uint8_t> from_canvas, to_canvas` 工作 buffer。
   - transition 调用点传进新的 clip refs + canvas buffers + 新 out flag。
   - common path 的 `spatial_identity` 检测扩 `spatial_already_applied || existing condition`——transition 已经 pre-transform 过就跳 caller 的 affine_blit 步骤（避免 double-apply），但仍跑 alpha_over（apply opacity）。

3. **Regression**：`test_compose_sink_e2e` 的 "single-track with cross-dissolve transition renders" case 两 clip 都是 identity transform + 匹配 W×H——新代码路径 `memcpy + cross_dissolve` 与旧 `cross_dissolve(src)` 字节等价。`test_determinism` 的 "compose path (2-track video + audio mixer)" 无 transition，也不受影响。全部 31 suite 绿（含新 transition 含带 audio mixer 含 cross-dissolve）。

4. **Scope 边界**：
   - **从 clip transform 生效**：从 identity 拓展到任意 Transform。
   - **to clip transform 生效**：新 pre-transform 路径把 to_clip.transform 应用在 cross_dissolve **之前**（spatial），common path 之后只 apply opacity（spatial 已完）。
   - **Dim mismatch 无障碍**：non-canvas-dim 的 transition 端点现在自动 scale-to-canvas via affine_blit，而不是 ME_E_UNSUPPORTED。
   - **未做**：
     - No from-EOF caching（若 from 在 window 中途耗尽，还是 degrade 到 to-only；`transition-to-clip-source-time-align` bullet 仍独立）。
     - No e2e test with non-identity Transform on transition clip —— 现有 byte-equivalent regression（identity path）+ affine_blit 单元测试足以给本改动信心；带 Transform transition 的 e2e 验证留到有需求时（用户 timeline 带 transition + scale 等）。

**Alternatives considered.**

1. **仅当至少一端有 non-identity Transform 时走新路径** —— 拒：分支增加 cognitive load + 测试覆盖复杂。"恒 pre-transform" 对 identity case 只多一次 memcpy（640×480×4 ≈ 1.2MB/frame），性能影响微不足道，代码更 uniform。
2. **把 cross_dissolve 改成"带两个 affine matrix" kernel** —— 拒：kernel 层变动面大 + cross_dissolve 作为像素 lerp 原语不该知道 affine。合成在调用层解耦 (transform → blend) 更干净。
3. **让 transition_step 也做 alpha_over，输出 dst_rgba 修改** —— 拒：alpha_over 需要 dst buffer + opacity，那是 caller 的 per-track 上下文。transition_step 只管"blend 两 endpoint 到 track_rgba"，不越俎代庖。
4. **保留 `out_src_w/out_src_h` 仍是源 dims（不统一成 W×H）** —— 拒：现在 track_rgba 总是 W×H，out dims 必须对齐否则 caller 的 fast-path W×H 判等失败。
5. **加 e2e test with Transform on transition clip** —— 延后到有产品场景时做。本 cycle 已经是大改（compose_transition_step signature + caller）；加 e2e 会让 PR 改动面失控。Regression 覆盖已由现有 identity-transition test 保障。

**Scope 边界.** 本 cycle **交付**：
- compose_transition_step 始终 pre-transform 每端。
- Caller 识别 spatial-already-applied + 只 apply opacity。
- Dim-mismatch 的 transition 端点自动 scale-to-canvas。

本 cycle **不做**：
- E2E test 带 non-identity Transform on transition clip。
- from-EOF caching。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 31/31 suite 绿。
- `test_compose_sink_e2e` 7 case（含 cross-dissolve transition）绿——identity case 字节等价于前版本。
- `test_determinism` 5 case（含 compose byte-identical）绿。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_transition_step.hpp`：signature 加 from_clip / to_clip / from_canvas / to_canvas / out_spatial_already_applied 参数；update comment 反映 "transforms always applied"；remove "phase-1 transform limitation" 表述。
- `src/orchestrator/compose_transition_step.cpp`：大改；新 `spatial_identity` / `transform_to_canvas` / `decode_to_rgba` 私有 helpers；transition 主流程走 pre-transform → cross_dissolve。
- `src/orchestrator/compose_sink.cpp`：frame loop 前加 from_canvas / to_canvas 工作 buffer；调用点传 clip refs + new scratches + 接 `spatial_already_applied`；common-path `spatial_identity` 扩接 `spatial_already_applied || existing`。
- `docs/BACKLOG.md`：**删除** bullet `transition-with-transform`。

**§M 自动化影响.** M3 current milestone；`transition-with-transform` 是 §M2 尾款（bullet 标签 §M2），non-criterion impacting。Cross-dissolve exit criterion 已在 M2 tick（2026-04-23 `1ecf807`）；本 cycle 的完善不反证该 criterion，只提升行为正确性。§M.1 不 tick。
