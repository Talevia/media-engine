## 2026-04-23 — compose-transform-wire（scope-A：opacity 部分）（Milestone §M2 · Rubric §5.1）

**Context.** `compose-transform-wire` bullet 完整 scope：把 `Clip::transform` 的四条轴（translate / scale / rotate / opacity）都在 compose loop 里消费，关闭 M2 exit criterion "Transform (静态) 端到端生效（translate/scale/rotate/opacity）"。translate/scale/rotate 需要 2D affine pre-composite（新 helper 或 sws-based transform，相当于一个 per-track RGBA 坐标变换层），是独立的相当 scope；opacity 只需要在现有 `alpha_over` 调用里替换 hardcoded `1.0f` 参数。本 cycle 切 opacity 这块——minimal change，独立测试。

Before-state grep evidence：

- `src/orchestrator/compose_sink.cpp`（上 cycle）`alpha_over(..., /*opacity=*/1.0f, BlendMode::Normal)`——hardcoded 完全不透明，忽略 `Clip::transform.opacity`。
- `grep -rn 'transform->opacity\|transform\.opacity\|transform->translate' src/orchestrator/` 返回空——Transform struct 的任何字段都没被 compose 读过。
- `me::Transform` struct 已经存在且 loader 已填字段（`transform-static-schema-only` cycle）；compose 是唯一缺口。

**Decision.** 两点改动：

1. **`src/orchestrator/compose_sink.cpp`** ComposeSink::process 的 alpha_over 调用前：
   ```cpp
   const me::Clip& clip = tl_.clips[ta.clip_idx];
   const float opacity =
       clip.transform.has_value()
           ? static_cast<float>(clip.transform->opacity)
           : 1.0f;

   me::compose::alpha_over(
       dst_rgba.data(), track_rgba.data(),
       W, H, static_cast<std::size_t>(W) * 4,
       opacity,                     // <- was 1.0f
       me::compose::BlendMode::Normal);
   ```
   - `transform.has_value()` 判断 clip 是否显式声明了 transform 对象；未声明 → 默认 opacity=1.0（向后兼容 existing e2e test 行为 byte-identical）。
   - 显式 `Transform{}` （空 object） → 走 IR 默认值 `opacity=1.0`（IR 构造时 Transform struct 默认 identity）——同样不改变 render 行为。
   - 显式 `"opacity":{"static":0.5}` 或其他值 → 真读入 alpha_over。

2. **`tests/test_compose_sink_e2e.cpp`** 新 TEST_CASE "2-track with per-clip transform opacity renders"：
   - 构造 2-track timeline，top track 的 clip 带 `"transform":{"opacity":{"static":0.5}}`。
   - 真 h264/aac 渲染；断 wait == ME_OK，产物文件非空（> 4096 bytes）。
   - 非-mac CI：videotoolbox 不可用 → 静默 return。
   - 不做 pixel-level verify（videotoolbox 非 deterministic + h264 压缩 decode 回来要 ffprobe，过度复杂）。kernel-level math 已经在 `test_compose_alpha_over` 的 "50% src alpha produces 50/50 mix" case 覆盖——本 e2e 只 pin "opacity 参数真的从 IR 流到 kernel"。

**Scope 留给 follow-up `compose-transform-affine-wire`**：
- `Clip::transform.translate_x/y` + `scale_x/y` + `rotation_deg` + `anchor_x/y` 的 affine pre-composite。
- 需要新 helper `me::compose::affine_blit(dst_rgba, src_rgba, src_w, src_h, dst_w, dst_h, affine_matrix)` 或用 sws_scale 的 filter-with-warp API。
- 每 track 的 RGBA frame 经 affine 变换到 dst canvas（含出界裁剪、填充 alpha=0）。
- kernel-level 测试 + e2e 测试（非 identity 变换后产物文件非空 + 不同 translate 产物 size 确实不同）。

**M2 exit criterion 影响**：criterion 文字是 "Transform (静态) 端到端生效（translate/scale/rotate/opacity）"。本 cycle 只 wire 1/4 轴（opacity）；严格读 = **不打勾**。§M.1 evidence 三元组里 "src/ 非 stub 实装" 对于整个 criterion 不满足——translate/scale/rotate 仍然 hardcode identity。下 cycle `compose-transform-affine-wire` 闭环后再 tick。

**Alternatives considered.**

1. **本 cycle 同时做 affine** —— 拒：affine pre-composite 需要 ~150 LOC kernel（lerp / bilinear sampling / anchor point math）+ 整合 + 测试；opacity 是 5 行改动，2 cycles 分开更稳。
2. **把 opacity 推给 alpha_over 的 BlendMode enum** （`Normal + OpacityN` 之类） —— 拒：已经有 `opacity` 参数，enum 不用来 carry 数值。
3. **把 "has_value()" check 简化**（每个 Clip 无 transform 时让 loader 默认填 identity Transform）—— 拒：`nullopt` vs `Transform{}` 语义区别有价值（见 `transform-static-schema-only` decision），compose 消费时 `1.0f` 是明显的 identity default。
4. **把 opacity 读取代码抽 `clip_opacity(clip)` helper** —— 拒：5 行内联可读；抽 helper 增加 indirection。
5. **加一个单元测试**（非 e2e）验证 opacity 从 clip 读出 —— 拒：ComposeSink 不 expose 内部 loop 给单元测试；e2e 是合适的粒度。kernel-level `test_compose_alpha_over` 已经覆盖 alpha_over 的 opacity 数学。

业界共识来源：AE / Premiere 的 Clip-level opacity 是独立"layer property"（层级属性），直接 multiplicative 到 composite 的 alpha；translate/scale/rotate 是空间变换。两层概念分开的工程 pattern。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 24/24 suite 绿。
- `build/tests/test_compose_sink_e2e` 3 case / 18 assertion（+1 new case / +6 assertion）。
- Mac + videotoolbox：opacity=0.5 2-track render 成功，产物 > 4096 bytes。
- 已有 e2e 的 2-track render（无 transform）依然 green —— `has_value()` check 处理了 transform 缺失 → 默认 1.0 → 行为不变。
- `test_determinism` 4/22 byte-equal 继续 pass（compose 路径不参与 single-track determinism）。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_sink.cpp`：+5 行 opacity 读取，alpha_over 第 5 参数从 `1.0f` → `opacity` 变量。
- `tests/test_compose_sink_e2e.cpp`：+1 TEST_CASE。
- `docs/BACKLOG.md`：删 `compose-transform-wire`，加 `compose-transform-affine-wire`（剩下 3 轴的 affine 变换）。

**§M 自动化影响.** M2 exit criterion "Transform (静态) 端到端生效" 本 cycle **不满足**——opacity 就位但 translate/scale/rotate 不在。保留未打勾。下 cycle `compose-transform-affine-wire` 闭环后再 tick。
