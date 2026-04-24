## 2026-04-23 — animated-transform-e2e：end-to-end render test with keyframed transform（Milestone §M3 · Rubric §5.1）

**Context.** `transform-animated-support` bullet last narrowed to two remaining items: (1) Clip::gain_db migration, (2) e2e test of animated transform rendering through compose path. Layer-3 Transform struct + compose integration landed last cycle (`transform-animated-integration`). The M3 exit criterion "animated property interpolation correct" was ticked on evidence from 3 unit-test suites (`test_animated_number` + `test_animated_number_loader` + `test_timeline_schema`) — the kernel + loader + parse-evaluate paths all exercised. But an e2e tripwire ensuring a **timeline with keyframed transform actually produces a render** — no compile-time type error, no runtime crash, no ComposeSink regression — was still missing.

Before-state grep evidence：
- `grep -n 'animated.*transform\|keyframes.*transform' tests/` 返回空——没 e2e 测 animated transform 通过 compose 管线。
- `test_compose_sink_e2e` 既有 cases 全部用 static transforms (空对象或 `opacity:{static:...}`)；没 case 走 `evaluate_at(T)` 非常规 per-frame 路径。

**Decision.**

1. **`tests/test_compose_sink_e2e.cpp`** +1 TEST_CASE "ComposeSink e2e: animated transform (translateX keyframes) renders end-to-end"：
   - 2-track timeline：`v0` 是 static baseline（覆盖画布），`v1` 带 `"transform":{"translateX":{"keyframes":[{t=0,v=0}, {t=25/25,v=200}], "interp":"linear"}, "scaleX":{"static":0.5}, "scaleY":{"static":0.5}}`。
   - v1 的 `scaleX:0.5` 让 compose_sink 走 **affine_blit 慢路径**（non-identity spatial），此时 translateX 的 `evaluate_at(T)` 值直接流入 `compose_inverse_affine`。若集成 broken，render 要么 error、要么产出异常（crash 或 stuck）。
   - 断言：`me_render_start` == ME_OK；`wait_s` == ME_OK（skip if videotoolbox 不可用）；产物存在 + size > 4KB。实测 **406008 bytes** —— 合理范围（compose 带 2 track + animated transform + h264/aac）。

2. **Not doing**：
   - 像素-准确 assertion：videotoolbox 非确定 + 内容依赖 compression，不实际。数值正确性由 `test_animated_number` + `test_animated_number_loader` + `test_timeline_schema` 的 3 层单测 pin 住。
   - Byte-identity 回归：同理，videotoolbox 非严格保证。`test_determinism` 的 "compose path byte-deterministic" 是 same-config 回归 tripwire；animated transform 的 byte 差异会自然被那个 case 捕获 if regression 发生（不同 Transform → 不同帧 → 不同 h264 bitstream → byte mismatch between runs 反而不 = 问题，byte **一致** between 同一 config 两 run 才是 criterion）。

3. **Scope 边界**：
   - **交付**：animated transform 的 end-to-end 渲染 tripwire。
   - **不做**：Clip::gain_db animated 迁移（仍 static-only，见 bullet narrow）。

4. **Bullet narrow**：删除 e2e-test 项，只剩 gain_db 迁移。gain_db 迁移是独立特性 scope（audio-pipeline 要接 T 参数），可能单独 bullet 化或留在本 bullet 细线收尾。

**Alternatives considered.**

1. **Byte-exact regression via 2-run compare** —— 拒：`test_determinism.cpp` 既有的 "compose path byte-deterministic" case 已提供 run-to-run stability tripwire；新 case 只验 "不 crash + 有合理输出" 就够。不 duplicate 现有覆盖。
2. **Add pixel-level assertion (e.g. top-left corner sampled at 3 frames)** —— 拒：从 compressed h264 取样本要 ffprobe 或 decode loop，test 复杂度上升；videotoolbox 输出在同 host 稳定但跨平台变。`test_animated_number` 数值精确、`test_compose_affine_blit` 空间精确，跨两层组合的纯数学已有 pin。
3. **Single-track test (no v0 baseline)** —— 拒：single-track + non-multi-track 会 route 到 make_output_sink 而不是 ComposeSink（has_transitions=false + tracks.size==1）。强制 2+ 轨让 ComposeSink 是被测对象。
4. **Test animated opacity instead of translateX** —— 拒：opacity 流到 alpha_over 的 `opacity` 参数，compose_sink 当前 fast path，不触发 affine_blit 代码路径；translateX 经 non-identity 驱动 affine 慢路径，覆盖 `evaluate_at` 更深的调用链。
5. **Animated transform with bezier cp instead of linear** —— 拒：linear 足够证明 evaluate_at 值流进 compose；bezier 的正确性单独由 `test_animated_number` 的 ease-in-out asymmetry 测试 pin。e2e 只需一种 interp。

**Scope 边界.** 本 cycle **交付**：
- compose e2e tripwire with keyframed translateX transform。

本 cycle **不做**：
- Clip::gain_db 迁移（留 bullet narrow）。
- 其它 interp 模式 e2e（linear 已覆盖；bezier/hold/stepped 等 interp 路径由 `test_animated_number` 单测 pin）。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 31/31 suite 绿。
- `test_compose_sink_e2e` 新增 1 case / 6 assertion（总 8 case）：animated-transform e2e render 406KB 输出。

**License impact.** 无。

**Registration.**
- `tests/test_compose_sink_e2e.cpp`：+1 TEST_CASE。
- `docs/BACKLOG.md`：bullet `transform-animated-support` narrow——e2e test 项删除；only gain_db 迁移留。

**§M 自动化影响.** M3 exit criterion "animated property interpolation" 已 tick（`transform-animated-integration` cycle, `6898082`）。本 cycle 是 regression-coverage enrichment，不影响 tick 状态。§M.1 无新变化。
