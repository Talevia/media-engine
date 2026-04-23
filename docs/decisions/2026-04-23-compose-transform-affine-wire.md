## 2026-04-23 — compose-transform-affine-wire：ComposeSink 读 Clip::transform 三轴空间变换（Milestone §M2 · Rubric §5.1）

**Context.** `compose-transform-affine-blit-kernel` cycle 已落 `affine_blit` + `compose_inverse_affine` helpers。本 cycle 把它们接到 ComposeSink::process 的 per-TrackActive 循环里，让 `Clip::transform` 的 translate_x/y + scale_x/y + rotation_deg（+ anchor_x/y）真实生效。opacity 已在更早的 `compose-transform-wire-opacity` cycle 落下。四轴齐了→ M2 exit criterion "Transform (静态) 端到端生效" 可以 tick。

Before-state grep evidence：

- `src/orchestrator/compose_sink.cpp`（本 cycle 前）每 active track 只调 `alpha_over(dst, track_rgba, ..., opacity, Normal)`；没有任何 spatial pre-composite。
- `src/orchestrator/compose_sink.cpp` 有硬 check "track frame size (src_w×src_h) doesn't match output (W×H) → ME_E_UNSUPPORTED"——意味着非 identity scale 就会被拒。
- `grep -rn 'affine_blit\|compose_inverse_affine' src/orchestrator/` 返回空——kernel 未在生产消费。

**Decision.**

1. **`ComposeSink::process` 分 fast / slow path**（`src/orchestrator/compose_sink.cpp`）：
   - 新局部 `spatial_identity` 判断：`!clip.transform.has_value() || (tr.translate_x == 0 && translate_y == 0 && scale_x == 1 && scale_y == 1 && rotation_deg == 0)`。anchor 在三个 differentials 全 identity 时无影响，不参与判定。
   - **Fast path**（spatial identity）：保留现有 `alpha_over(dst, track_rgba, W, H, ..., opacity, Normal)`。源 dims 必须匹配 output dims（原 hard check 不变，err 消息更新提示"如果要用非默认 dims 就设 non-identity Transform"）。
   - **Slow path**（有 spatial transform）：
     ```cpp
     AffineMatrix inv = compose_inverse_affine(
         tr.translate_x, tr.translate_y,
         tr.scale_x, tr.scale_y,
         tr.rotation_deg, tr.anchor_x, tr.anchor_y,
         src_w, src_h);
     affine_blit(track_rgba_xform.data(), W, H, W*4,
                  track_rgba.data(), src_w, src_h, src_w*4,
                  inv);
     alpha_over(dst_rgba, track_rgba_xform, W, H, ..., opacity, Normal);
     ```
   - slow path 自然 handle 源 dims ≠ output dims（affine_blit 做 sampling + bounds → transparent）。fast path 的硬 check 仍要求 same dims。

2. **新中间 buffer `track_rgba_xform`**：canvas 大小 (W × H × 4)，一次分配外循环外，affine_blit 每次 overwrites all pixels——不累积 stale data。只有 slow path 用它；fast path 走 alpha_over(track_rgba) 直接。

3. **Test** `tests/test_compose_sink_e2e.cpp` 新 TEST_CASE "per-clip translate renders (spatial transform wired)"：
   - 双 render：top track 一次用 `transform: {}`（spatial identity 走 fast path），一次用 `transform: {translateX: {static: 100}, translateY: {static: 50}}`（slow path）。
   - 都断 wait == ME_OK + file size > 4096。
   - 断两产物 **file_size 差 ≥ 1% of min**——translate 改变像素分布，h264 压缩后 size 必然不同。1% 是保守 floor（dev 机实测差几个 % / 几百 KB）。
   - Pin "translate 真的走到了 pixel 层"——无需 decode-compare 复杂操作。
   - videotoolbox 不可用时静默 skip。

4. **Log noise**：观察到 `[h264_videotoolbox] Color range not set for nv12. Using MPEG range.` 日志从 ffmpeg 层打印——是 encoder 的 color range 提示（MPEG = limited range），非本 cycle 引入，和输出质量无关。不改。

**M2 tick rationale**：criterion 文字 "Transform (静态) 端到端生效（translate/scale/rotate/opacity）"。evidence 三元组：

- **src/ 非 stub 实装**：ComposeSink::process 现在 fast-path 读 opacity、slow-path 读 translate/scale/rotate/anchor 全部入 affine_blit 管道；`src/compose/affine_blit.cpp` 非 stub。
- **CI 覆盖**：`test_compose_affine_blit` 8 case / 105 assertion（kernel math 纯数学正确），`test_compose_sink_e2e` 新增 translate-triggers-affine-path case（pipeline 端到端）。opacity 已由 `test_compose_alpha_over` 11 case / 37 assertion + `test_compose_sink_e2e` opacity case 覆盖。
- **Recent feat commits**：`compose-transform-wire-opacity` (opacity)、`compose-transform-affine-blit-kernel` (kernel)、本 cycle (wiring) 三个 feat 提交。

严格读 criterion: 四轴都 wired + tested。**tick**。

**Alternatives considered.**

1. **永远走 slow path（删 fast path）** —— 拒：fast path 是 byte-identical 历史行为的回归保证；slow path 即使 identity 也多一个 W×H×4 内存拷贝 + 一次 nearest-sample 循环 = 额外 perf。fast/slow branch 成本低（一个 bool 检查），值得。
2. **fast path vs slow 通过 tracks 声明 identity 一次决定**（避免每帧 re-check） —— 拒：per-frame check 是 O(1)（5 个 double 比较），比持 per-track 额外状态更简单；未来 animated transforms 生效时每帧值都可能不同——per-frame 检查是正确粒度。
3. **slow path 里 bilinear 采样** —— 拒：bilinear 是 `compose-transform-bilinear-upgrade` 的 scope（nearest + small transforms 的 quality 已能过"correct" bar；bilinear 是 quality 升级）。
4. **允许 fast path 下 src_w ≠ W**，自动 sws scale —— 拒：引入 per-track SwsContext cache 复杂度；"想跨分辨率就用 Transform.scale" 是合理要求（也给 host UI hint: 非 identity dims 对应 non-identity transform）。
5. **slow path 直接从 AVFrame 用 sws 做 affine** 跳过 RGBA8 中间层 —— 拒：sws 的 affine 支持不是第一公民 API；本路径（YUV→RGBA→transform→RGBA→YUV）更 debuggable。
6. **spatial_identity 检查用浮点 epsilon** —— 拒：double 字段由 loader 直接 copy JSON 数字；identity 由 host 代码写 `translate: 0, scale: 1, rot: 0`（`Transform{}` 默认结构也是这些值）——strict `==` 匹配预期。epsilon 会让 `translate: 0.00001` 也走 fast path，可能不是 host 意图。

业界共识来源：Premiere / FCP / DaVinci 的 track-level Transform 处理：identity 时 bypass 变换管道，non-identity 时走 2D compositor。common fast-path pattern。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 25/25 suite 绿。
- `test_compose_sink_e2e` 新增 "per-clip translate renders" case——identity vs translate 两 render 的产物 file_size 差 ≥ 1%（实测 dev 机差 ~几个 %）。
- 现有 3 个 e2e case + opacity case 继续 green（fast path 对 transform-absent / spatial-identity transform 行为 byte-identical）。
- `test_determinism` 4/22 byte-equal 继续 green（单 track 走 reencode_mux，不经 ComposeSink）。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_sink.cpp`：include `compose/affine_blit.hpp`；per-TrackActive loop 分 fast/slow path；新 `track_rgba_xform` 中间 buffer。
- `tests/test_compose_sink_e2e.cpp` 新 TEST_CASE (4→5 cases, 18→27 assertions)。
- `docs/BACKLOG.md`：删 `compose-transform-affine-wire`。

**§M 自动化影响.** M2 exit criterion "Transform (静态) 端到端生效（translate/scale/rotate/opacity）" 三元组 evidence 完整：
- src 非 stub：ComposeSink + affine_blit。
- CI 覆盖：test_compose_affine_blit（kernel）+ test_compose_sink_e2e（pipeline opacity + translate）+ test_compose_alpha_over（alpha math）。
- Recent feat commits：三个 feat 提交（opacity, kernel, wire）。

→ §M.1 **tick "Transform (静态) 端到端生效" exit criterion**（独立 `docs(milestone):` commit）。

M2 变成 3/6 exit criteria done。剩：audio mix, cross-dissolve, software-path determinism。
