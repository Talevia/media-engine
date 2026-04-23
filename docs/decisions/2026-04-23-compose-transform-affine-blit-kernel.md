## 2026-04-23 — compose-transform-affine-blit-kernel（scope-A：affine blit + inverse matrix builder）（Milestone §M2 · Rubric §5.1）

**Context.** 上 cycle opacity 已 wire，但 Transform 的 translate/scale/rotate 还没。`compose-transform-affine-wire` 全 scope：kernel（2D affine 变换 RGBA buffer）+ 集成 ComposeSink + kernel + e2e tests。本 cycle 切 kernel 部分——纯数学 helper + 单元测试。ComposeSink 集成留 follow-up。

Before-state grep evidence：

- `grep -rn 'affine_blit\|AffineMatrix\|compose_inverse_affine' src/` 返回空。
- `me::Transform` struct 的 translate_x/y / scale_x/y / rotation_deg / anchor_x/y 七字段在 loader 已填（`transform-static-schema-only` cycle），但只有 `opacity` 被 compose 消费（上 cycle）。

**Decision.** 新 `src/compose/affine_blit.{hpp,cpp}`：

1. **`struct AffineMatrix`**：row-major 2×3 affine（a, b, tx, c, d, ty）。默认构造 = identity。映射 `(x, y) → (a·x + b·y + tx, c·x + d·y + ty)`。blit kernel 用它的**逆矩阵**（canvas → src 的 backward sampling）。

2. **`AffineMatrix compose_inverse_affine(translate_x, translate_y, scale_x, scale_y, rotation_deg, anchor_x, anchor_y, src_w, src_h)`**：
   - 构造 Transform 语义的 **逆** 矩阵——forward 顺序是 shift-to-anchor-origin → scale → rotate → shift-anchor-back → translate；逆矩阵撤销这五步反向顺序。
   - anchor 以 normalized 0..1 解释（`anchor_x * src_w` = pixel-offset）。
   - rotation_deg 顺时针，θ = deg * π/180，`cos(θ)` + `sin(θ)` 由 `<cmath>` 提供。
   - degenerate: `scale_x == 0 || scale_y == 0` → 返回 identity（zero-scale = 合理 hidden，但 inverse 是 singular；identity sampling 会让 dst 输出 src 内容但 wrong orientation——是 bug-detectable 的 sentinel，不崩）。
   - Output matrix 公式：`a = cos(θ) / scale_x`、`b = sin(θ) / scale_x`、`c = -sin(θ) / scale_y`、`d = cos(θ) / scale_y`，plus translation offsets derived from anchor + translate。

3. **`void affine_blit(dst, dst_w, dst_h, dst_stride, src, src_w, src_h, src_stride, const AffineMatrix& inv)`**：
   - Iterate dst pixel by pixel。
   - 用 inv 把 dst (x, y) 映射到 src (sx, sy) 浮点坐标。
   - `lroundf` 做 nearest-neighbor 取整（deterministic，cross-host 一致）。
   - Bounds check: 在 src range → copy RGBA pixel；出界 → 写 `{0, 0, 0, 0}`（transparent black），让下游 alpha_over 把该像素当作"该层不参与合成"。
   - dst / src 别名是 UB（kernel 依赖 src 完整读取不被覆盖）。

4. **Tests**（`tests/test_compose_affine_blit.cpp`，8 TEST_CASE / 105 assertion）：
   - Identity matrix → dst pixel-for-pixel copy of src。
   - Pure translate (+2, +1) → dst (5, 3) carries src (3, 2) labels；dst (0, 0) OOB 透明。
   - Pure scale 2× → 4×4 src → 8×8 dst，dst (2, 2) ← src (1, 1)，dst (4, 4) ← src (2, 2)。
   - Large out-of-bounds translate → entire dst transparent (OOB verify).
   - 2×2 src → 4×4 dst identity → src data at dst (0..1, 0..1)，其余 transparent。
   - 180° rotation around anchor center → forward src (x, y) → canvas (W-x, H-y)，inverse sampling 验证 dst (3, 3) ← src (1, 1)（continuous-rotation semantics；anchor 在 (2, 2) = normalized 0.5 × 4 所以 (0, 0) 映射到 (4, 4) OOB，和 "discrete 180° flip" intuition 不同——test 用 math-faithful 断言）。
   - Determinism: 非平凡 transform 跑两次，byte-identical。
   - Test labeled src: each pixel R=x G=y B=0 A=255 so dst values directly identify source-of-sample。

5. **Debug loop**：第一版的 180° rotation 测试 expected dst (3,3) 从 src (0,0) 来（"image flipped"）——fail。重算：continuous 180° around (2, 2) maps src (0, 0) → (4, 4) OOB；src (1, 1) → (3, 3)；dst (3, 3) ← src (1, 1)，值是 (1, 1) not (0, 0)。修 test 断言匹配数学；decision 里记录 "discrete-flip convention 需要 anchor ((W-1)/2, (H-1)/2)"——用户层应该知道。

**Follow-up**：
- `compose-transform-affine-wire`（**新 bullet**，下一 cycle 或下下 cycle）：在 ComposeSink::process 内 per TrackActive 用 `compose_inverse_affine(Clip::transform.*)` 得 matrix，`affine_blit(intermediate_rgba, transformed_rgba)` 到 canvas-size 中间 buffer，再 `alpha_over(dst, intermediate, opacity, Normal)`。新 e2e test 验证非 identity translate 产物 != identity 产物。
- `compose-transform-bilinear-upgrade`（未来 perf）：nearest → bilinear sampling 提升 scale / rotate 画质（4× arithmetic per pixel）。

**Alternatives considered.**

1. **Bilinear 采样** 这 cycle 一起做 —— 拒：bilinear 需要 4-tap weighted avg + fractional coord 管理，~3× kernel 代码量；nearest 对所有 blank-alpha regression tripwire 已足够。quality follow-up bullet。
2. **用 sws_getContext 的 filter-with-warp API** 替 own kernel —— 拒：sws 的 affine 支持是非 canonical（要 hack 用 `sws_setColorspaceDetails`），且 portable 性差；own kernel + determinism + LGPL 独立。
3. **Row-major 3×3 matrix** 而非 2×3 —— 拒：2D affine 的第三行永远 `[0, 0, 1]`，存 3×3 浪费；2×3 是 OpenGL / Core Graphics / Cairo 惯例。
4. **API 直接接 Transform struct** 而非拆参数 —— 拒：`compose_inverse_affine` 是纯数学 helper，和 Timeline IR 解耦更灵活（未来 transitions / effects 可能也要算 affine）。ComposeSink::process 里做 Transform → affine 的 adapter 一行代码。
5. **scale=0 时 return `std::optional<AffineMatrix>` nullopt** —— 拒：增加 API 的两路径处理；caller 其实很少 scale=0；fallback 到 identity + 让 kernel 输出可能怪但不崩是 pragmatic 选择。
6. **float 精度改 double** —— 拒：像素坐标 ~1000 级，float32 mantissa 23-bit 远足够；double 双倍内存+慢。

业界共识来源：Adobe AE / Premiere Pro 的 Transform 语义（anchor-based scale + rotate + position）；Cairo `cairo_matrix_t` 的 2D affine design；OpenCV `warpAffine` 的 inverse-matrix + sampling pattern。本 cycle 全部沿袭。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 25/25 suite 绿（新 `test_compose_affine_blit` 是第 25）。
- `build/tests/test_compose_affine_blit` 8 case / 105 assertion / 0 fail。

**License impact.** 无。

**Registration.**
- `src/compose/affine_blit.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` 追加 source。
- `tests/test_compose_affine_blit.cpp` + `tests/CMakeLists.txt` `_test_suites` 追加 + include dir。
- `docs/BACKLOG.md`：删 `compose-transform-affine-wire`（当前 bullet），加 `compose-transform-affine-wire` 收紧版（kernel 就位，剩 wiring + e2e）。

**§M 自动化影响.** M2 "Transform (静态) 端到端" 本 cycle 不 tick——kernel 就位但 ComposeSink 未接；等 `compose-transform-affine-wire` cycle 闭环。
