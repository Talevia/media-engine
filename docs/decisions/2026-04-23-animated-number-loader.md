## 2026-04-23 — animated-number-loader：layer 2 of transform-animated-support（JSON → AnimatedNumber parse）（Milestone §M3 · Rubric §5.1）

**Context.** `transform-animated-support` 有 3 层。第 1 层（IR primitive + 插值数学 + 单测）已由 `animated-number-primitive` cycle 落地。本 cycle 切第 2 层：JSON `{"keyframes": [...]}` 形式的 loader 解析，填 `me::AnimatedNumber`。Caller 迁移（第 3 层）留后续。

Before-state grep evidence：
- `grep -rn 'parse_animated_number\b' src/` 返回空（现仅有 `parse_animated_static_number`，返 double，拒 keyframes 形式）。
- `src/timeline/timeline_loader.cpp:86` 前身版本 + `src/timeline/loader_helpers.cpp` 的 `parse_animated_static_number`：`if (prop.contains("keyframes")) throw ME_E_UNSUPPORTED "phase-1..."`。
- TIMELINE_SCHEMA.md §Animated Properties 定义 keyframes 形式 `{t, v, interp, cp?}` + 约束（sorted by t, no dup, bezier cp x1/x2 ∈ [0,1]）。

**Decision.**

1. **`src/timeline/loader_helpers.hpp`** + `loader_helpers.cpp` 新增 `parse_animated_number(json, where) → me::AnimatedNumber`：
   - 接受 `{"static": <num>}` 形式 → `AnimatedNumber::from_static(v)`。
   - 接受 `{"keyframes": [...]}` 形式。每 keyframe 解 `{t(rational), v(number), interp(string), cp(optional 4-array)}`。
   - Validation 合约：
     - Exactly-one of {static, keyframes}（两者并存或都缺均 ME_E_PARSE）。
     - `keyframes` 非空数组。
     - 每 kf 必须有 `t`/`v`/`interp`；`t` 是 `{num,den}` 通过 `as_rational` 验证（den > 0）；`v` 是 number；`interp` 是 `"linear"|"bezier"|"hold"|"stepped"`。
     - `cp` **必须**存在且 `interp=="bezier"` 时；4-float array；cp[0]/cp[2] ∈ [0, 1]（CSS cubic-bezier 单调性前提——`AnimatedNumber::bezier_y_at_x` 依赖）。
     - 不认识的 kf key → ME_E_PARSE。
     - Keyframes 严格 sorted by t + no-duplicate → ME_E_PARSE on violation（i64 cross-multiply 精确比较）。
   - Static 形式保留 `parse_animated_static_number` 独立 helper 不变——现在 Transform / Clip::gain_db 仍调它（caller 迁移是第 3 层工作）。

2. **`parse_interp`** 私有 helper（anon namespace in loader_helpers.cpp）：string → `me::Interp` enum，未知值 ME_E_PARSE。

3. **Tests** (`tests/test_animated_number_loader.cpp`) —— 13 TEST_CASE / 27 assertion：
   - Positive：static / linear / bezier-with-cp / hold / stepped 都正确 parse 成对应 AnimatedNumber（含 evaluate_at 验证一条 linear midpoint）。
   - Negative：empty `{}`、static+kf 并存、unknown interp、bezier 缺 cp、cp x1 超 [0,1]、unsorted t、duplicate t、空 kf 数组、unknown kf key。
   - 统一用 throw + catch pattern（helpers 抛 `LoadError` 异常，本测试直接断 `LoadError::status`）。

4. **Scope 边界**：
   - **不**改 `parse_animated_static_number`（现有行为保持——拒 keyframes with "transform-animated-support" 错误消息）。
   - **不**迁移 `parse_transform` 或 Clip::gain_db 使用新 helper（第 3 层工作）。
   - **不**动 `me::Transform` 的 8 `double` 字段——仍 double（第 3 层才换 AnimatedNumber）。
   - 用户当前仍然无法在 JSON 里用 `{"keyframes":[...]}` 给 transform / gain_db 赋值——loader chain 到 `parse_transform → parse_animated_static_number → 拒`。本 cycle 的 `parse_animated_number` 是**孤立的 helper**，只对直接调它的 caller（第 3 层之后的 parse_transform migrated version）有效。

**Alternatives considered.**

1. **Replace `parse_animated_static_number` → `parse_animated_number`，caller 要 `.evaluate_at(t=0)` 得到 double** —— 拒：破坏性变动，所有 caller 一次性迁移。Layered migration 更安全。
2. **Helper 里 `static + keyframes` 都存时取 static 优先** —— 拒：明确 ambiguity 优于 silent 优先级选择。
3. **`interp` 字段可选（默认 linear）**—— 拒：schema 明文 `interp` required。
4. **Cp 约束 y1/y2 也限制 [0,1]** —— 拒：CSS cubic-bezier 允许 y1/y2 超出（overshoot / anticipation effects，如 "back easing"）。x1/x2 必须 [0,1] 是数学合法的单调性约束；y1/y2 只要有限即可。
5. **去 `interp_str_to_enum` 共享 table** —— 拒：anon namespace 里一个 helper 函数足够；四 enum 值 memorable。
6. **让 `as_rational` 接受 negative t** —— 已经验证 `den > 0` 而不是 `num > 0`，所以 negative t 技术上解析通过。本 cycle 选择 **不**额外加"t >= 0" 断言——keyframes 的第一个 t 通常是 0 但 schema 不强制；用例中 `t.num` 是可负的（composition 有负 time 起点的奇异情况）。未来有具体场景再约束。

**Scope 边界.** 本 cycle **交付**：
- `parse_animated_number` helper（支持 static + keyframes 两种形式）。
- 13 单元测试。

本 cycle **不做**：
- Caller 迁移（`parse_transform` / Clip::gain_db）。
- Transform struct 的 double → AnimatedNumber 迁移。
- Compose 路径的 evaluate_at 接入。

Bullet `transform-animated-support` **保留**，narrow 反映 loader 层已就位；剩层 3（caller 迁移 + Transform 改 AnimatedNumber + compose 求值）。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 31/31 suite 绿（+1 suite `test_animated_number_loader`）。
- `test_animated_number_loader` 13 case / 27 assertion。

**License impact.** 无（pure C++ / nlohmann::json already linked）。

**Registration.**
- `src/timeline/loader_helpers.hpp`：+ `parse_animated_number` decl + `#include "timeline/animated_number.hpp"`。
- `src/timeline/loader_helpers.cpp`：+ `parse_interp` private helper + `parse_animated_number` impl。
- `tests/test_animated_number_loader.cpp`：新文件。
- `tests/CMakeLists.txt`：+ test suite + include + `nlohmann_json::nlohmann_json` link。
- `docs/BACKLOG.md`：bullet `transform-animated-support` narrow——loader 层已就位，剩 caller 迁移 + Transform / compose 接入。

**§M 自动化影响.** M3 exit criterion "所有 animated property 类型的插值正确" 本 cycle **未满足**——loader 能 parse 了但没 caller 用它；end-to-end animated timeline 渲染还未通。§M.1 不 tick。
