## 2026-04-23 — transform-animated-integration：Transform 字段换成 AnimatedNumber + loader/compose caller 迁移（Milestone §M3 · Rubric §5.1）

**Context.** Layer 3 of `transform-animated-support`。Layer 1 (IR + 插值数学) + Layer 2 (JSON parse) 已就位。本 cycle 迁移 Transform IR + 所有 caller：
- `me::Transform` 的 8 `double` 字段 → `me::AnimatedNumber`。
- 新 `me::TransformEvaluated` struct 存一次 evaluate 出来的 8 doubles。
- Loader `parse_transform` 用 `parse_animated_number`（支持 static + keyframes）。
- Compose 路径（compose_sink.cpp + compose_transition_step.cpp）在 frame loop 调 `transform.evaluate_at(T)` 取当前值。

Before-state grep evidence：
- `src/timeline/timeline_impl.hpp:82-91`：8 fields 都是 `double = 0.0 / 1.0 / 0.5`。
- `src/timeline/loader_helpers.cpp:parse_transform`：`read(key, t.translate_x)` → 调 `parse_animated_static_number` 得 double。
- `src/orchestrator/compose_sink.cpp:385-395`：`clip.transform->opacity` / `clip.transform->translate_x == 0.0` 直接字段访问。
- `src/orchestrator/compose_transition_step.cpp:25, 85`：`c.transform->translate_x` 等字段直接访问。
- `tests/test_timeline_schema.cpp`：`t.translate_x == 100.0` 等直接比较 double。

**Decision.**

1. **`src/timeline/timeline_impl.hpp`** 结构改造：
   - `#include "timeline/animated_number.hpp"`。
   - 新 `struct TransformEvaluated { 8 doubles }` + `spatial_identity()` method。
   - `struct Transform`：8 字段从 `double` 换成 `AnimatedNumber`，默认 `AnimatedNumber::from_static(...)`（identity 值）。
   - `Transform::evaluate_at(me_rational_t t) → TransformEvaluated` 方法。

2. **`src/timeline/loader_helpers.cpp::parse_transform`**：
   - `read(key, target)` lambda 从 `double&` 改成 `me::AnimatedNumber&`。
   - Read 调 `parse_animated_number`（支持 static + keyframes 两形式）。
   - Opacity range validation 改为 `validate_opacity` lambda：static 形式验证单值；keyframes 形式验证每 kf.v 都在 [0,1]。

3. **`src/orchestrator/compose_sink.cpp`** frame loop：
   - `const me::TransformEvaluated tr_eval = clip.transform.has_value() ? clip.transform->evaluate_at(T) : TransformEvaluated{};`
   - 替换 `clip.transform->opacity` → `tr_eval.opacity`；
   - 替换 spatial-identity 检查 → `tr_eval.spatial_identity()`；
   - 替换 affine 读 → `tr_eval.translate_x` 等。
   - 删除局部 `const me::Transform& tr = *clip.transform;` 引用（tr_eval 提供所有值）。

4. **`src/orchestrator/compose_transition_step.hpp/cpp`** signature 换：
   - 从 `const me::Clip& from_clip, const me::Clip& to_clip` 改成 `const me::TransformEvaluated& from_tr, bool from_has_transform, const me::TransformEvaluated& to_tr, bool to_has_transform`。
   - Caller 在 frame loop（有 T）里 evaluate 出 TransformEvaluated 再传进。
   - 内部 `transform_to_canvas` + `spatial_identity_for` 相应改成取 TransformEvaluated。

5. **`tests/test_timeline_schema.cpp`** 3 处更新：
   - "clip transform empty object → populated with identity defaults"：读 `tv = t.evaluate_at({0,1})` 再断 `tv.translate_x == 0.0` 等。
   - "clip transform with static translateX / opacity / rotationDeg parses to IR"：同样转 evaluate_at 后再断。
   - "clip transform with keyframes rejected" 旧测试 **删除 + 替换**：now "clip transform with keyframes (animated) now parses and evaluates per-T"——正向断言 keyframes form 解析成功 + linear interp at 0/15/30 得 0/100/200。
   - 其他 opacity-out-of-range / unknown-key / `Transform{}`-constructibility tests 不受影响（都不直接读 double 字段）。

6. **Regression**：现有 28 个 Transform-users ignore cases (不带 Transform 的 timeline) 都零改动影响 —— `clip.transform.has_value() == false` 仍走 identity 默认路径，行为字节等价。`test_determinism` 的 "compose path byte-identical" 案例无 Transform →确认 byte-equivalent。

**Alternatives considered.**

1. **保留 `double` fields + 加 sidecar `optional<AnimatedNumber>` 字段** —— 拒：两字段并存需逻辑分叉 + 内存翻倍 + loader 要选哪个填。单一 AnimatedNumber 字段 + evaluate_at 清晰。
2. **Transform 改 union/variant 支持 both double-static + AnimatedNumber** —— 拒：`AnimatedNumber::from_static` 本身就是对 "pure double" 的轻量包装（optional<double> + vector<Keyframe>，静态时 vector 为空零-cost）。variant 开销 > 收益。
3. **不加 TransformEvaluated，直接 8 次 `.evaluate_at(T)` in caller** —— 拒：每 frame 每 clip 8 次 look up + 8 次 eval_at 的 call site 重复 8 次；TransformEvaluated 一把算完一次，code tidy。
4. **Keyframes animated transform 的 per-clip schema validation** (e.g. kf.t 必须落在 clip 的 `time_start` 区间内) —— 拒：schema 写 "composition time not clip-local"（见 TIMELINE_SCHEMA.md:221），kf.t 是 composition-time，可以超出单 clip 的 time_start..end 范围（AnimatedNumber 前/后段的 clamp 取首/末 kf.v 成立）。本 cycle 不加这个约束。
5. **同 cycle 迁移 Clip::gain_db** —— 拒：AudioMixer 目前 gain apply 在 AudioTrackFeed open 时 freeze 为常量；让 gain 动态化需要 mixer API 接 T 参数 + 每 frame eval gain，scope 比 Transform 迁移大得多（mixer / track_feed / 可能 audio pipeline 串行化）。独立 bullet。
6. **同 cycle 加 e2e 测试带真 keyframes transform** —— 拒：已经改 6 文件；新 e2e 要建 with-keyframes fixture timeline + render + verify 视觉结果。schema 单测（test_timeline_schema）覆盖了 parse + evaluate_at 的正确性；e2e 等用户有 demo 时加。

**Scope 边界.** 本 cycle **交付**：
- Transform IR 全 AnimatedNumber 化。
- Loader 解 keyframes 形式的 transform。
- Compose 路径 evaluate_at(T) 替代直接字段读。
- 3 现有 Transform tests 更新到 evaluate_at pattern + 替换 "keyframes rejected" 为 "keyframes parses + evaluates"。

本 cycle **不做**：
- Clip::gain_db 迁移（layer 3 剩余部分）。
- E2e 测试带真 keyframes transform 渲染。

Bullet `transform-animated-support` narrow——Transform + compose caller 已就位；剩 gain_db 迁移 + e2e test。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 31/31 suite 绿。包括现有 47 test_timeline_schema cases + 新 keyframes-transform 解析/求值断言（替换旧 rejection 测试）。
- `test_animated_number` 11 case + `test_animated_number_loader` 13 case + `test_timeline_schema` transform-related cases 全部覆盖端到端 animated-transform 路径（JSON → parse → Transform → evaluate_at）。

**License impact.** 无。

**Registration.**
- `src/timeline/timeline_impl.hpp`：+ `#include animated_number.hpp` + `struct TransformEvaluated` + Transform fields 改 AnimatedNumber + `evaluate_at` 方法 + doc-comment 更新。
- `src/timeline/loader_helpers.cpp:parse_transform`：调 `parse_animated_number` 代替 `parse_animated_static_number`；opacity validation 扩到每 kf.v。
- `src/orchestrator/compose_sink.cpp`：`tr_eval` 引入替代 `clip.transform->*` 访问。
- `src/orchestrator/compose_transition_step.hpp/cpp`：signature 从 `const me::Clip&` 两个 → `const TransformEvaluated&` + `has_transform` flag 两对；内部 helpers 跟着改。
- `tests/test_timeline_schema.cpp`：3 cases 迁到 `.evaluate_at({0,1})` 访问；一 case "keyframes rejected" 换成 "keyframes parses + evaluates"。
- `docs/BACKLOG.md`：bullet `transform-animated-support` narrow——Transform 层 + loader + compose 求值已完，剩 Clip::gain_db 迁移 + e2e test。

**§M 自动化影响.** M3 exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" —— 已就位：
- IR 类型支持 4 种插值（layer 1）。
- Loader parse keyframes + validate（layer 2）。
- Transform 字段为 AnimatedNumber + compose 每帧 evaluate_at（本 cycle）。
Evidence triple 审核：
- src 非 stub impl：AnimatedNumber + Transform + parse_transform + ComposeSink evaluate_at 都是实装。
- CI 覆盖：test_animated_number（4 插值）+ test_animated_number_loader（parse）+ test_timeline_schema（end-to-end）三层都绿。
- Recent feat commits：animated-number-primitive / animated-number-loader / 本 cycle 三连。

**可 tick**。§M.1 本 cycle 后独立 `docs(milestone):` commit。
