## 2026-04-23 — transform-static-schema-only：Clip transform 静态 schema 落地（Milestone §M2 · Rubric §5.1）

**Context.** M2 exit criterion "Transform (静态) 端到端生效（translate/scale/rotate/opacity）" 包含两部分：(a) timeline schema 承认 transform、loader 填入 IR；(b) compose 阶段读 transform 实际变换像素。(b) 依赖 multi-track-video-compose P1 bullet。本 cycle 只做 (a)：schema 落地 + IR 存储 + 严格拒绝 out-of-scope 输入（animated form 和未知 key），让 compose cycle 不用同步改 loader 正则。

Before-state grep evidence：

- `grep -n 'transform\|Transform' src/timeline/timeline_impl.hpp` → 没 `Transform` 结构；`Clip` 只有 `asset_id` / `time_*` / `source_start` 四个字段。
- `grep -n 'transform' src/timeline/timeline_loader.cpp` → 唯一命中 `line 193-194` 的 `require(!clip.contains("transform"), ME_E_UNSUPPORTED, "phase-1: clip.transform not supported")`——旧的硬拒绝。
- `docs/TIMELINE_SCHEMA.md:160-175` 已定义 Transform 的 JSON 形状（animated-property wrappers + 8 字段 translateX/Y, scaleX/Y, rotationDeg, opacity, anchorX/Y）；loader 没实现对应解析。

**Decision.** 三个文件的最小闭环改动：

1. **`src/timeline/timeline_impl.hpp` 增 `struct Transform`**：8 个 `double` 字段（translate_x/y, scale_x/y, rotation_deg, opacity, anchor_x/y），默认值就是 identity（Transform{} 合法）。`Clip` 新增 `std::optional<Transform> transform` 字段：`nullopt` = JSON 压根没写 `transform`；`Transform{}` = 写了 `transform: {}` 但字段全默认。保留这个区分让 downstream 可以对"clip 真的没变换"做 fast-path。IR 层用 `double` 因为 translate/scale/rotation/opacity 天然连续，不涉及帧对齐——CLAUDE.md 不变量 #3 "Time is rational" 只约束**时间**，不约束像素/角度/比例值。

2. **`src/timeline/timeline_loader.cpp` 新 `parse_transform` + `parse_animated_static_number` 两个 helper，替换旧硬拒绝**：
   - `parse_animated_static_number(prop, where)`：解析一个动画属性 wrapper，要求 `{"static": <number>}` 形式；遇到 `{"keyframes": [...]}` 就 `ME_E_UNSUPPORTED`（并在错误 message 里引用 `transform-animated-support` backlog item）；缺 `static` 键也 `ME_E_PARSE`。
   - `parse_transform(j, where)`：校验 j 是 object；逐字段检查每个 key 是否在 8 个已知 key whitelist 里，未知 key → `ME_E_PARSE "unknown transform key 'X'"`；每个已出现的字段走 `parse_animated_static_number`；最后检查 `opacity ∈ [0, 1]` → 越界 `ME_E_PARSE`（其他字段任意有限 double，允许负 scale 做镜像）。
   - 删除 `line 193-194` 的 `require(!clip.contains("transform"), ME_E_UNSUPPORTED, ...)`。
   - 在 clip 构建处加 `if (clip.contains("transform")) c.transform = parse_transform(clip["transform"], where + ".transform");`。

3. **`tests/test_timeline_schema.cpp` 增 8 个 TEST_CASE**：
   - absent `transform` → `.transform == nullopt`
   - empty object `{}` → `.transform.has_value() && == Transform{}`（identity defaults）
   - 全字段 static 存入 IR 正确（translateX=100, translateY=-50, scaleX=2, rotationDeg=45, opacity=0.5，未指定的 scaleY/anchorX 保持 identity）
   - `keyframes` 形式 → `ME_E_UNSUPPORTED`，错误 message 含 "animated" + "keyframes"
   - `opacity: {static: 2.0}` → `ME_E_PARSE`，含 "opacity"
   - 未知 key（`skew`）→ `ME_E_PARSE`，含 "unknown transform key" + "skew"
   - `{"static": {nested:1}}` object-shaped value → `ME_E_PARSE`（非数字）
   - `{"translateX":{}}` 缺 `static` 键 → `ME_E_PARSE`

**Not done in this cycle**（scope gate）：

- **Compose path 不接 transform**。Exporter / Previewer / Orchestrator 的任何 code path 都不读 `Clip::transform`。passthrough stream-copy 不能变换像素（本来就 identity），reencode path 现在也 ignore（即使 JSON 设了 translate=100，输出也不动）。实际 compose 合成需要 `multi-track-video-compose` P1 bullet 先落地 2D blit / composite 内核。
- **Animated properties 不支持**。M3 milestone exit criterion 有专门的"所有 animated property 类型的插值"工作；本 cycle 留下严格 UNSUPPORTED 拒绝 + 下面的 follow-up backlog item。
- **CompositionThumbnailer 不接 transform**（它自己还是 STUB，见 CLAUDE.md "Known incomplete"）。

**Stub 自查（rule 7）.** 本 cycle 新增一条 `ME_E_UNSUPPORTED` 返回点（`parse_animated_static_number` 的 keyframes 分支）。同时消除一条（`transform` 整体拒绝）。**净 ±0**。规则 7 允许这种"为未来 scope 留 gate"的 UNSUPPORTED，前提同 commit 里加 backlog bullet——本 cycle 追加 `transform-animated-support` 到 P2（SKILL §6 允许的 debt append，1 条，上限 2）。

**Alternatives considered.**

1. **引入 `me::AnimatedNumber` variant 类型立刻**（`std::variant<Static, Keyframes>` 或 `struct AnimatedNumber { bool animated; double static_value; std::vector<Keyframe> keyframes; };`），loader 完整解析两种形式，这 cycle 只 validate keyframes 但不动画插值 —— 拒：IR 改动过大（每个 future animated property 的 consumer 都得接 variant），而本 cycle 只 ask schema+IR 就绪，M3 拆 animated 出来是更合理的 scope 边界。双路径工作是必要的 duplication：本 cycle 一条 `double`，M3 一条 `AnimatedNumber` 替换 `double`——替换是机械的，不产生架构重写。
2. **用 nested `translate: {x, y}` object 而非 flat `translateX / translateY`** —— 拒：`docs/TIMELINE_SCHEMA.md:160-175` 已定义 flat 形式，schema doc 是真理源。改 schema doc 会牵涉更多（downstream JSON 示例、language binding 预期）。
3. **JSON 里出现 `transform` 但是空 object → 当 nullopt 处理（和 absent 等价）** —— 拒：`nullopt` vs `Transform{}` 的区分有意义（详见 struct 注释）。对于未来"真无变换 fast-path" vs "显式写了但默认"的差异敏感。
4. **在 loader 里额外校验 rotation_deg ∈ [-360, 360]**（避免 host 传奇怪值） —— 拒：rotation modulo 360 在任何 compose 实现里都是常识；强行收窄 range 限制 host 的自由（host 可能想表达 "转 720°，过 2 圈"做动画）。opacity 的 [0,1] 范围是另一个层面——out-of-range 会破坏 alpha blending 的定义，因此**必须**校验。
5. **schema 加 `"mode":"static"` 标记字段，统一 static vs animated** —— 拒：现在 schema 用的是 "key 是 `static` 还是 `keyframes`" 做判定——等价于 tagged union with 2 tags，不需要显式 mode string。
6. **把 transform 字段直接存为 nlohmann::json** 避免解析开销 —— 拒：IR 的目的之一是消除 JSON library 依赖出 timeline/ 模块；compose 消费方 include nlohmann/json 就破圈。typed struct 才是规范。
7. **Transform 字段用 float（32-bit）节省内存** —— 拒：8 × 4 byte × N clips = 可忽略内存；double 保 90°旋转 + translate 精度无亏。M3 GPU 路径如果 SIMD 要 float，那时 narrow-cast 或者让 GPU-specific 路径用另一个 POD 即可，IR 不降精度。

业界共识来源：OTIO 的 Transform 字段（translation / rotation / scale / anchor，但 OTIO 是 Python flat-objects；媒体引擎 C++ IR 更贴 AE/Motion/Premiere 的 "transform property" model）、CSS Transform 规范（translate/scale/rotate/opacity 作为独立可动画属性）、Motion Graphics 领域的 "each property keyframeable independently" 约定。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 16/16 suite 绿，整体 3.45s。
- `build/tests/test_timeline_schema` 25 case / 122 assertion（先前 17 case / ~90 assertion；+ 8 case / +32 assertion）。0 failed 0 skipped。
- 现有 15 个 suite 都不受影响（Transform 在 Clip 结构中新增 `std::optional<Transform>`，默认 nullopt；Exporter / Previewer 不读 `Clip::transform`，不影响 passthrough / reencode 路径）。
- 确定性回归（test_determinism）不受影响——传进来的 timeline 没 transform，IR 里 `.transform == nullopt`，sink 行为字节一致。

**License impact.** 无。

**Registration.**
- `src/timeline/timeline_impl.hpp`：新 `me::Transform` struct，`me::Clip::transform` 字段。
- `src/timeline/timeline_loader.cpp`：新 `parse_animated_static_number` + `parse_transform` helper，删旧硬拒绝，clip 构建处 wire。
- `tests/test_timeline_schema.cpp`：+8 TEST_CASE。
- `docs/BACKLOG.md`：删 `transform-static-schema-only` bullet；P2 末尾 append `transform-animated-support`。

**§M 自动化影响.** M2 exit criterion "Transform (静态) 端到端生效" **尚未** 完全满足——本 cycle 只做 schema+IR，端到端 compose 要等 `multi-track-video-compose` bullet 落地后才 e2e 验证。因此本 cycle 的 §M.1 evidence 三元组里"src/ 非 stub 实装"条缺（compose 还没读 transform），exit criterion 保留未打勾。
