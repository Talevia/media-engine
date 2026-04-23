## 2026-04-23 — debt-timeline-builder-negative-tests：timeline loader 5 条剩余拒绝路径的回归覆盖（Milestone §M2-prep · Rubric §5.2）

**Context.** `tests/test_timeline_schema.cpp` 已有的 negative case 覆盖：schemaVersion != 1、malformed JSON、非连续 clip（overlap）、multi-track、clip.effects、null engine、last_error populated. 但 loader 里还有几条 rejection path 完全没 CI 覆盖，bullet 指的那几条：

- **schemaVersion 缺失**（不是显式的 2，而是根本没字段）——loader 走 `doc.value("schemaVersion", 0)` 默认路径，理论上 fail 在 `== 1`。这条 default-value 路径改起来最容易滑过。
- **assets 空 / track.clips 空**——loader 有 `require(!clips.empty(), ME_E_PARSE, ...)`，但没 test。
- **unknown assetId**——`tl.assets.find(id)` miss 时 loader 抛 "unknown asset"，没 test。
- **duration.den == 0**——`as_rational` 的 `den > 0` 约束，决定 "divide by zero 不 leak 到 downstream"，没 test。
- **output.compositionId 指向不存在的 composition**——loader 遍历 compositions 找 matching id，没找到 `require(comp != nullptr, ...)`，没 test。

每条都是 loader 的 fail-fast contract，缺 CI tripwire 意味着**任何一条被无意识移除**都会滑过 M1 build。本 cycle 补齐。

**Decision.** `tests/test_timeline_schema.cpp` 加 5 个 TEST_CASE，插在 `phase-1 rejects multi-track timeline` case 之前（为了 schema-level 负 case 聚在一起，不拆成 sibling `test_timeline_schema_negative.cpp`——已有 suite 的上下文足够 close，新 TU 徒增 CMake 登记）：

1. **missing schemaVersion → ME_E_PARSE + "schemaVersion"**
2. **empty track.clips → ME_E_PARSE + "at least one clip"**
3. **unknown assetId → ME_E_PARSE + "unknown asset"**
4. **timeRange.duration.den == 0 → ME_E_PARSE + "den must be > 0"**
5. **output.compositionId 指向不存在 composition → ME_E_PARSE + "unknown composition"**

所有 case 都用 raw JSON inline（而非通过 TimelineBuilder）。理由：这些是**故意不合法**的 JSON shape，TimelineBuilder 被设计成产生合法 JSON，为 5 条 negative case 扩它的 API 加 "schema_version_omit"、"clips_empty"、"duration_den_zero" 等 mutator 只增复杂度不增收益。Raw JSON ~20 行 / case 简单直读、偏离已有 positive pattern 小。

每条都按**语义 token** 断言 err 子串（沿用 `docs/TESTING.md` 里记的 pattern）——cosmetic reword 不挂，删语义 token 才挂。

**本 cycle 跳过 `debt-timeline-loader-engine-seed-pattern`.** P1 的 top-1 原本是它。bullet 的 direction 明确写 "等 M2 第二种 seed 资源（color pipeline / effect LUT 预热）出现再定"——trigger 条件（第二个 consumer）不存在时做任何设计 decision 都是 premature（缺数据、两条路无法比较）。按 skill 的精神"被跳过的 bullet 保留不动"，不动它，跳到下一条 P1 `debt-timeline-builder-negative-tests` 处理。下一次 repopulate 要是 seed 资源第二个 consumer 还没出现，这个 bullet 应该考虑改成"明确等 X 触发"的 parked 形态（或直接挪到 P2）以反映 "当前不是 actionable" 的状态。

**Alternatives considered.**

1. **新建 `tests/test_timeline_schema_negative.cpp`**——拒：现有 `test_timeline_schema.cpp` 已经按 case-level 标题组织正 / 负混合了 ~12 个 case，继续在同一文件加 5 个 close-topology 的 case 比新 TU 干净（一次 test_ 文件登记有成本：CMake + include 声明 + test_main 重复链接）。
2. **扩 TimelineBuilder 支持 schemaVersion-omit / empty-clips 等 mutator**——拒（见上，ROI 低）。
3. **用 `doctest::SUBCASE` 把 5 个 case 聚在一个 TEST_CASE 里参数化**——拒：每个 rejection 的 fixture JSON 差别大（不同字段缺 / 不同值），共享 setup 节省不了代码量。平铺 TEST_CASE 让 ctest 输出 "哪个 negative case 挂了" 一眼可见。
4. **也测 colorSpace 未识别字符串 rejection**——考虑过；loader 确实 reject `primaries: "bogus"` 之类，但 err 串稳定性 depends on libav enum name list，**跨 FFmpeg 版本**可能漂移。本 cycle 不碰，等真需要时用 allowlist test helper 覆盖。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 12/12 suite 绿。
- `build/tests/test_timeline_schema -s` 17 case / 72 assertion（从 12/50 升）。
- 不动 src/，其他 11 个 suite 继续绿。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。仅 `tests/test_timeline_schema.cpp` 加 5 个 TEST_CASE。
