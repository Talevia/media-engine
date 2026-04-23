## 2026-04-23 — cross-dissolve-kernel（scope-A：RGBA8 线性 lerp）（Milestone §M2 · Rubric §5.1）

**Context.** 本来应该做 top P1 `multi-track-compose-actual-composite`（把 ComposeSink 的 bottom-only delegation 换成真 alpha_over per-frame loop），但该工作的最小可闭环 scope 仍是 ≥ 1 整 cycle 的 architectural integration（encoder+mux setup + per-track decoder state + per-output-frame 调度），在一次 cycle 内做不完。过去三四个 cycle 已经多次"rotation 到 sibling P1"来 side-step 这类 too-big 任务；本 cycle 继续这个模式——pick `cross-dissolve-kernel` bullet 的 scope-A 数学内核部分。

Before-state grep evidence：

- `grep -rn 'cross_dissolve\|CrossDissolve' src/` 只命中 `timeline/timeline_impl.hpp` 里的 `TransitionKind::CrossDissolve` enum 值（从 `cross-dissolve-transition` cycle 来）——**没有**像素级合成代码。
- `me::compose::alpha_over` 存在（支持 opacity），但"从 `from` 渐变到 `to`"的 lerp pattern 不是 alpha_over 的合约（alpha_over 是 A over B，不是 `lerp(A, B, t)`）。
- `src/compose/` 目录下 3 个已有 TU（alpha_over / active_clips / frame_convert）；cross_dissolve 是自然的第 4 个。

**Decision.** 同 `multi-track-compose-kernel` 对 alpha_over 的处理方式：纯数学 free function + 独立 TU + 单元测试。

1. **`src/compose/cross_dissolve.hpp/cpp`**：
   - `void cross_dissolve(uint8_t* dst, const uint8_t* from, const uint8_t* to, int w, int h, size_t stride, float t)`
   - 数学：`dst[i] = round(from[i] * (1-t) + to[i] * t)` per channel RGBA8。
   - `t` clamp 到 [0,1]。
   - 2D 双 for-loop，`lroundf` 做 round-to-nearest-away-from-zero（和 alpha_over 保持一致，跨-host deterministic）。
   - 要求 `dst` 与 `from`/`to` 不别名（UB if aliasing——lerp 依赖读源字节未变）。
   - Alpha 通道也 lerp，不是 bit-preserve（两端 opaque 输出仍 opaque；两端部分透明则 alpha 线性插值）。

2. **与 `alpha_over` 的关系**：两个函数合作但合约不同：
   - `alpha_over(dst, src, ..., opacity, BlendMode)`：把 src **叠加**到 dst（Porter-Duff over）。
   - `cross_dissolve(dst, from, to, ..., t)`：从 from 线性**过渡**到 to。
   - 在 cross-dissolve transition 实际 compose 流程里：从 from 和 to 两 clip 各自解码一帧 → 调 `cross_dissolve` 得混合帧 → 喂 encoder（没有 dst 背景，是两端都"主角"）。
   - 等价性：`cross_dissolve(dst, from, to, t)` 也可以由 `memcpy(dst, from); alpha_over(dst, to, ..., t, Normal)` 近似，但结果略不同——alpha_over 走 Porter-Duff 会把 from 的 alpha 纳入，cross_dissolve 直接对 4 通道 per-channel lerp。transition 语义是后者（两边都是 opaque 的 final-render 帧）。

3. **Tests**（`tests/test_compose_cross_dissolve.cpp`，9 TEST_CASE / 28 assertion）：
   - t=0 → dst = from exact；t=1 → dst = to exact（端点 identity）。
   - t=0.5: (0+255)/2=127.5→128；从(100,100,100,255)到(200,200,200,255)的 0.25 → 125，0.75 → 175（典型 lerp values）。
   - t 超出 [0,1] clamp 验证（-0.5→0, 1.5→1）。
   - 4 channel 独立（绿通道在 from=to 情况下保持；其他通道各自 lerp）。
   - Determinism：相同 input+t 跑两次 byte-identical。
   - Zero-size buffer no-op（no crash）。

4. **Follow-up**: 剩余 `cross-dissolve-kernel` scope（bullet 原文）：decode from-clip 尾段 N 秒 + to-clip 头段 N 秒，linear alpha ramp 驱动 cross_dissolve 逐帧调用，输出到 encoder。需要的是 compose-sink-level 的 per-frame 调度（类似 multi-track-compose-actual-composite 要做的工作，只不过是"两 clip 相邻"而非"两 track 平行"）。新 P1 bullet `cross-dissolve-transition-wire` 承接。

**Alternatives considered.**

1. **让 cross_dissolve 复用 alpha_over**（`memcpy from` + `alpha_over to with opacity=t`）—— 拒：alpha_over 走 Porter-Duff，from 的 alpha 影响结果；transition 语义需要直接 per-channel lerp 不考虑原 alpha。独立 impl 合约清晰。
2. **不 lerp alpha 通道，保持 from 的 alpha** —— 拒：一端透明一端不透明的跨叠加场景下 alpha 不过渡就会 pop。per-channel lerp 包括 alpha 是 NLE 惯例（Premiere / FCP / DaVinci 的 cross dissolve 对 straight alpha source 是这样的）。
3. **SIMD / AVX 加速** —— 拒：M2 correctness-first；perf 留到有 profile 证据后。
4. **premultiplied alpha 路径** —— 拒：VFX premul path 是 future 话题；M2 straight alpha。
5. **把 `cross_dissolve` 塞进 `alpha_over.cpp`** —— 拒：两个 kernel 的合约 + 数学不同，独立 TU 更 self-documenting。
6. **4-channel fixed-point 整数 lerp** 替代 float —— 拒：`(a*(255-t_u8) + b*t_u8 + 127)/255` 会有 1 LSB 舍入差异和 float path 不一致；float lerp 和 alpha_over 一致性更好（两者都用 float32 + lroundf）。
7. **t 支持 out-of-range 不 clamp**（允许 extrapolation）—— 拒：扩大 [0,1] 之外的值产生过曝 / 欠曝，数学有定义但语义对 transition 不适用。clamp 是保守默认。
8. **把 cross_dissolve 作为方法挂在 `BlendMode` enum 里**——拒：BlendMode 是 alpha_over 的 parameter enum；cross-dissolve 不是 blend，是 transition 合约，不同概念。

业界共识来源：Premiere "Cross Dissolve" video transition 的数学（Adobe docs: "linear interpolation of each channel value"）、DaVinci Resolve 的 "Cross Dissolve" Fusion node、OpenFX `OfxDissolveTransition` 规范——都是 per-channel linear lerp。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 21/21 suite 绿（新 `test_compose_cross_dissolve` 是第 21 个）。
- `build/tests/test_compose_cross_dissolve` 9 case / 28 assertion / 0 fail。
- 其他 20 suite 全绿；新 TU 独立，无副作用。

**License impact.** 无新 dep。

**Registration.**
- `src/compose/cross_dissolve.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` `media_engine` source list 追加 `compose/cross_dissolve.cpp`。
- `tests/test_compose_cross_dissolve.cpp` 新 suite + `tests/CMakeLists.txt` `_test_suites` 追加 + `target_include_directories PRIVATE src/` block。
- `docs/BACKLOG.md`：删 `cross-dissolve-kernel`，P1 末尾加 `cross-dissolve-transition-wire`（真的把 kernel 接到 decoder → kernel → encoder 路径）。

**§M 自动化影响.** M2 exit criterion "Cross-dissolve transition" 本 cycle **未完成**——数学就位但 render 路径没接。§M.1 evidence 三元组里 "src/ 非 stub 实装" 不满足（Exporter transitions gate 仍 UNSUPPORTED）；criterion 保留未打勾。
