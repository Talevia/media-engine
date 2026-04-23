## 2026-04-23 — docs-testing-conventions：落地 `docs/TESTING.md`（Milestone §M2-prep · Rubric §5.2）

**Context.** `tests/` 现在 12 个 doctest suite + 1 个 fixture generator + 一组**未成文**的约定：fixture 复用（`ME_TEST_FIXTURE_MP4` + `add_dependencies(... determinism_fixture)`）、两种 skip 模式（fixture 缺失 / HW 不可用）、RAII handle guards、内部头通过 per-suite `target_include_directories` 暴露、err 子串断言、doctest `REQUIRE` vs `CHECK` 的分工习惯。过去 10 个 cycle 写每个新 suite 都要重新 re-derive 这些 pattern（我自己在 `debt-test-multi-track-asset-reuse` cycle 踩 TimelineBuilder 默认值 mismatch 就是这个问题）。继续不写文档的成本是未来每个 test 作者 lose ~30 分钟 re-derive 时间。

**Decision.** 新 `docs/TESTING.md`，按 backlog bullet 的 direction 指定的 4 大板块写：

1. **Build + run** — `ME_BUILD_TESTS=ON` / `ME_WERROR=ON` 推荐组合，单 suite verbose 跑 `-s`。
2. **Framework choice rationale + 基本 API** — doctest 选型理由（vs Catch2 / gtest），`REQUIRE` vs `CHECK` 分工（"precondition → REQUIRE；claim being tested → CHECK；多 CHECK 优于复合 REQUIRE"）。
3. **目录布局 + 三种 fixture pattern**：
   - **TimelineBuilder**（fluent JSON）：用法 + 四个 rational 字段同时覆盖的踩坑预警。
   - **`ME_TEST_FIXTURE_MP4`**（共享构建时 MP4）：CMake + C++ 双侧 boilerplate 贴出来。
   - **RAII handle guards**：EngineHandle / TimelineHandle / InfoHandle / PngBuffer 全列出来。
4. **两种 skip pattern**：fixture 缺失模板、HW 依赖缺失模板（mac h264_videotoolbox vs linux）。
5. **额外收入的 pattern**（这些过去 cycle 里反复出现但 bullet 没点名）：
   - Reach into `src/` 的 per-suite include 模式 + 为什么不项目级暴露。
   - 错误消息按**子串**断言（cosmetic reword 不挂 test，删语义 token 才挂）。
   - 字节比较 vs 像素比较的取舍（`test_determinism` slurp-and-compare + fail-with-offset；`test_thumbnail` 的 inline PNG header parse 替代 libpng）。
6. **加 / 不加 test 的判别表** —— 按"场景 → 该加 / 不该加"枚举：decision-backed contract → 该加；factory 无 consumer 的 inline → 该加；copying 规格（等同 FFmpeg arithmetic）→ 不该加。

写在 `docs/TESTING.md` 新文件里而不是塞进 `docs/ARCHITECTURE.md` 或 `CLAUDE.md`——testing 是一个独立的 concern 领域（和 architecture / operating rules 正交），独立文件让新贡献者搜"怎么写 test"能直接命中。

**Alternatives considered.**

1. **写进 `CLAUDE.md`**——拒：CLAUDE.md 是 operating-rules 中心，塞 3 页 test pattern 会淹没架构 invariants。
2. **写进 `tests/README.md`**——考虑过；路径更短但 `docs/` 是已有的单一文档仓集中点（`VISION.md` / `ARCHITECTURE.md` / `PAIN_POINTS.md` / `MILESTONES.md` / `INTEGRATION.md` / `TIMELINE_SCHEMA.md` / `API.md` / 现在 + `TESTING.md`）。保持形状一致。
3. **拆成多份文档**（`docs/TESTING_FIXTURES.md` + `docs/TESTING_SKIP.md` + ...）——拒：over-engineering。单文件 200 行，新 contributor 一口读完受益最多。
4. **只写"快速开始"不写 FAQ / "不该加 test 的场景"**——拒：最常见的反模式正是"加太多 test 把 refactor 锁死"，记一笔明确告知读者"不该 mirror specs" 价值 ≥ "告知读者怎么加 test" 价值。
5. **等 `TESTING.md` 被真正新 contributor 读过一遍再调**——本 cycle scope 是**落地**文档。下一个新 contributor 反馈点再迭代是正常 doc-evolution 流程，不拖到那时再写。

**已有 pattern 没写进来的.** 两类刻意略过：

- **`--force` / amend / rebase 相关的 git convention**——属于 operating rules（CLAUDE.md 已覆盖），不是 test-specific。
- **"如何 debug 失败的 ctest"**（lldb / print 路径）——偏 workflow，不是 convention；留给下一 cycle 如果出现"新贡献者卡这里"的信号再补。

业界共识来源：React / TypeScript / LLVM 都有独立 `TESTING.md`，pattern 是 "framework rationale + fixture patterns + when to test + when not to test + FAQ"。本文件沿这一模板。

**Coverage.**

- 纯 docs，新 `docs/TESTING.md` 约 150 行 markdown。
- `cmake --build build` 不受影响；`ctest` 12/12 继续绿（没 touch src / tests）。
- CLAUDE.md 的 "Read order" 将来可能补一条 `docs/TESTING.md` 的 ref——不在本 cycle scope（CLAUDE.md edit 属于 `debt-claude-md-*` 系 bullet 的职责范畴）。

**License impact.** 无。

**Registration.** 新增 `docs/TESTING.md` 一文。无代码 / CMake / API 变更。
