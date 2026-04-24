## 2026-04-23 — animated-number-primitive：scope-A of transform-animated-support（M3 exit criterion "animated interpolation"）（Milestone §M3 · Rubric §5.1）

**Context.** Bullet `transform-animated-support` 涉及三层：
1. 新 `me::AnimatedNumber` 类型 + 4 种插值模式。
2. Loader 解 `{"keyframes": [...]}` 形式（现在硬拒）。
3. `me::Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber` + compose 路径时间求值。

一个 cycle 塞不下所有 3 层。本 cycle 切**第 1 层**——IR primitive + 插值数学 + 单元测试——为后续 loader / Transform 改造铺路。

Before-state grep evidence：
- `grep -rn 'AnimatedNumber\|class Animated\|Keyframe' src include` 返回空。
- `src/timeline/timeline_loader.cpp:86` 的 `parse_animated_static_number` 硬拒 `"keyframes"` 形式（TIMELINE_SCHEMA.md §Keyframes 定义的 JSON shape）→ ME_E_UNSUPPORTED。
- `docs/TIMELINE_SCHEMA.md:213-218`：keyframes 形式 `{t, v, interp, cp?}`；interp ∈ {`linear`, `bezier`, `hold`, `stepped`}；cp 是 CSS cubic-bezier 四控制点。

**Decision.**

1. **`src/timeline/animated_number.hpp`** —— 新 IR 类型：
   - `enum class Interp { Linear, Bezier, Hold, Stepped }` + `struct Keyframe {t, v, interp, cp[4]}` + `struct AnimatedNumber {static_value opt, keyframes vec}`。
   - Factory helpers `from_static(v)` / `from_keyframes(kfs)`。
   - `evaluate_at(me_rational_t t) → double` 方法。

2. **`src/timeline/animated_number.cpp`** —— `evaluate_at` 实装：
   - Static → 直接返回。
   - 空 keyframes → `0.0`（defensive；caller 应validate）。
   - `t < front.t` → front.v（no extrapolation per schema）。
   - `t >= back.t` → back.v。
   - Single kf → kf.v anywhere。
   - 否则找 bracketing pair `[a, b]` 做 linear scan（kf 数 typical < 20；worth-profile 再升级 binary search）。
   - 按 `a.interp` 分支：
     - `Hold` / `Stepped` → `a.v`（numeric 语义下两者等价；enum distinction 留给 UI）。
     - `Linear` → `a.v + u * (b.v - a.v)`，`u = fraction(t, a.t, b.t) ∈ [0,1]`。
     - `Bezier` → `bezier_y_at_x(u, cp)` 返 u 的 y 值，再 lerp `a.v, b.v`。
   - `fraction(t, a, b)` 用 i64 cross-multiply 得精确分数，clamp [0,1]。
   - `bezier_y_at_x` 用 Newton-Raphson 8 迭代反解 x(s)=target_x，然后求 y(s)。CSS cubic-bezier 公式：`x(s) = 3(1-s)²s·x1 + 3(1-s)s²·x2 + s³`（同理 y）。x1/x2 clamp 到 [0,1] 确保 x(s) 单调。

3. **Tests** (`tests/test_animated_number.cpp`) —— 11 TEST_CASE / 34 assertion：
   - Static passthrough（t irrelevant）。
   - Empty keyframes → 0.0。
   - Single kf → kf.v。
   - Before first kf → first.v。
   - After last kf → last.v。
   - Linear midpoint + 1/3 point（精确分数）+ segment start。
   - Hold：segment 内全 a.v，下一 kf 时间 → b.v。
   - Stepped：同 hold 行为（numeric 语义）。
   - Bezier identity cp (0,0,1,1) → 与 linear 等价（endpoints exact + midpoint ~0.5）。
   - Bezier CSS ease-in-out (0.42, 0, 0.58, 1)：midpoint 对称 ~0.5；x=0.25 → y < 0.25（ease-in 慢起）；x=0.75 → y > 0.75（ease-out 加速尾段）。
   - Multi-segment chain 混合 Linear / Hold / Linear：每段行为正确。

**Alternatives considered.**

1. **`std::variant<double, std::vector<Keyframe>>`** —— 拒：variant 需要 `std::visit` / holds_alternative 模板，对 C++20 compile time 不便；optional + vector 双字段更简单透明。"静态 XOR 关键帧"语义由 caller 保证，不值得语言支持。
2. **Bezier 用 de Casteljau 算法 vs Newton** —— 拒：de Casteljau 需更多浮点运算。Newton 8 迭代对 CSS-style cp（x1/x2 ∈ [0,1]）快速收敛；profile 未证瓶颈。
3. **Interp 按字符串存 IR 内** —— 拒：VISION §3.2 + CLAUDE.md typed params 硬规——enum class 唯一合理选择。
4. **Keyframe 用模板化 v（支持 double / vec2 / color）** —— 拒：phase-1 只要 double；vec2 / color 加完 schema spec 再说；不要 premature generics。后续如果有 `AnimatedVec2`，可以抽 `AnimatedProp<T>` concept。
5. **Hold / Stepped 真 distinguish（stepped = jump at b.t 而 hold = 保持 a.v）** —— 拒：单段间两者等价，UI 意图差别不在数学层面。坦白注释"同 hold 行为"，避免未来 regressioner 误读。
6. **parse loader changes 同 cycle 做** —— 拒：scope 爆炸。loader 改 = schema 解 `keyframes` array 每 element 的 `t / v / interp / cp` + 各种 validation（sorted t, no dup, bezier cp 有效范围, etc.）。独立 bullet。
7. **一次性 Transform migration + loader + evaluate 全套** —— 拒：3 层混做 cycle 超 500+ LOC；debt 在 test_compose_sink_e2e 失败时找 regression 源码域极广。分层落地每层单测 < 100 LOC，更健康。

**Scope 边界.** 本 cycle **交付**：
- `AnimatedNumber` IR + 4 种插值。
- Unit tests。

本 cycle **不做**：
- Loader 解析 `keyframes` 形式（仍 UNSUPPORTED）——留 `transform-animated-loader` 后续 bullet。
- Transform struct 的 double → AnimatedNumber 迁移——留 `transform-animated-integration` 后续 bullet。
- Compose 路径在 time 求值 animated transform——同上 bullet 覆盖。

Bullet `transform-animated-support` **不删**——3 层只完 1。narrow 文本反映 primitive 已就位。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 29/29 suite 绿（+1 suite `test_animated_number`）。
- `test_animated_number` 11 case / 34 assertion。
- Total 28→29 suite。

**License impact.** 无。

**Registration.**
- `src/timeline/animated_number.hpp`：新文件。
- `src/timeline/animated_number.cpp`：新文件。
- `src/CMakeLists.txt`：+ `timeline/animated_number.cpp`。
- `tests/test_animated_number.cpp`：新文件。
- `tests/CMakeLists.txt`：+ test suite + src include。
- `docs/BACKLOG.md`：bullet `transform-animated-support` narrow——primitive 层已就位，剩 loader + Transform 迁移。

**§M 自动化影响.** M3 exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 本 cycle **未满足**——primitive 数学已实装 + 测试，但**尚未**接入 timeline / Transform / compose。Evidence 要求 end-to-end（timeline 里有 animated transform → render 时 eval 出正确帧），primitive 单测不够。§M.1 不 tick。
