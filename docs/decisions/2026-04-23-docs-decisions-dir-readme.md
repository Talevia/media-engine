## 2026-04-23 — docs-decisions-dir-readme：decisions/README.md 增 "Finding a decision" 段（Milestone §M2-prep · Rubric §5.2）

**Context.** `ls docs/decisions/` 当前 74 条决策文件，每月还增加约 15 条。文件名格式是 `<YYYY-MM-DD>-<slug>.md`，找"上次为啥这么做"非常依赖知道 slug 的 kebab-case 命名——新贡献者（或一周没看的本人）基本只能靠 `ls` 肉眼扫。现有 `docs/decisions/README.md` 只讲"怎么写决策文件 + 模板"，完全没覆盖"怎么找决策"。

Grep evidence：

- `ls docs/decisions | wc -l` → 74。
- `grep -l 'Finding' docs/decisions/README.md` 返回空——"find a decision" 的 narrative 完全缺失。
- `grep -l 'Rubric §5.1' docs/decisions/*.md | wc -l` → 实测有用，决策文件头部 `(Milestone §Mx · Rubric §5.y)` 是稳定 anchor。

**Decision.** 在 `docs/decisions/README.md` 末尾 append `## Finding a decision` 小节（~100 行），包含：

1. **开头 preamble**：点明 74 files、slug 知识缺口、"这节列实际起作用的搜索模式"定位。
2. **`### By module / topic`**：11 个 `grep -l` 命令覆盖主要模块 anchor——timeline IR / orchestrator / render / io / graph+task+scheduler+resource / color+OCIO / cache / public C API / build-CMake / tests-doctest / docs 编辑。每条 grep pattern 用 alternation（`\|`）把该模块的多个文件名 / 类名 / 符号一网打尽。所有命令用 `docs/decisions/*.md` glob（比 `docs/decisions/` 目录形式在各 platform grep 更稳，BSD grep 不一定自动 recurse）。
3. **`### By rubric axis`**：直接 grep `Rubric §5.X`——因为每个决策 header 按 SKILL.md 模板都写了 rubric tag。§5.1 / §5.2 / §5.3 各一例，§5.4+ 按 VISION 表扩。
4. **`### By milestone`**：grep `Milestone §M1 / §M2` 以及 `ls ... | sort -r | head -20` 按时间近期。
5. **`### By decision keyword`**：专门针对"我们考虑过 X 吗？为什么拒了 X？"查询——利用每份决策的 `**Alternatives considered.**` 段。提供 3 个具体业界名字例子（x264 / Taskflow / SAX）——这些刚好在决策里出现过，是查 alternatives 体感的 dry-run。`grep -l -F 'Alternatives considered' ... | xargs grep -H -F '<keyword>'` 的管道组合是本节最重的单条。
6. **`### Common question → starting file`**：6 行 Q→A 表，把新贡献者最常见的 6 种"engine 怎么工作 / 为什么这样"问题直接点到具体决策文件 + 补充 docs（`ARCHITECTURE_GRAPH.md`、`CLAUDE.md "Known incomplete"`）。
7. **`### If all else fails`**：收尾兜底，IDE 全文搜素 + 承认 grep pattern 的 UX 上限是"缩到 ~5 文件而非一个"。

**为什么选 grep-based 查询而非 INDEX.md 自动汇总.** Bullet direction 写 "Or 在 `docs/decisions/` 加 `INDEX.md` 分类汇总"——拒这条。原因：

- INDEX.md 是**衍生数据**，每 cycle 落地决策 + 追加 INDEX 条目两处改才能保持同步；iterate-gap 规则 "decision 文件只新建不编辑" 不容忍在同 commit 里既加 decision 又 edit INDEX。
- 74 → 200 条时 INDEX 本身变成滚动 TOC，需要 diff-aware 维护，成本随规模线性增长。
- `grep` is the index——每个决策 header 已经是自描述格式（rubric + milestone + slug），grep 就是 pull-based 查询，零维护成本。
- Git 历史如果决策被 rename / move（理论上不会发生，但合并 branch 场景可能），grep 立刻跟上；INDEX 会 stale。

**Alternatives considered.**

1. **`INDEX.md` 按 rubric 分类汇总** —— 拒，理由上面展开。
2. **把 find 指引放到 CONTRIBUTING.md** —— 拒：decisions/README.md 是"这个目录怎么用"的权威 doc，找决策的指引跟写决策的指引并列最有上下文。
3. **在每个决策 header 加 tag 字段如 `tags: [timeline, schema]`** —— 拒：header 格式已经够长（`## <date> — <title> (Milestone §Mx · Rubric §5.y)`），再加 tags 要改 74 条历史决策，向后修改。
4. **用 shell script / Makefile target 封装常用 grep**——如 `make find-decision MOD=timeline`——拒：脚本封装抢 grep 的直接性；CLAUDE.md 风格偏"展示命令，不展示 wrapper"。
5. **直接给 rubric axis 写 description 在 README 里**（§5.1 是什么 / §5.2 是什么） —— 拒：rubric 定义是 `docs/VISION.md` §5 的 authoritative 范围；README 只指向 anchor。
6. **加 ReferenceLink 到 GitHub decisions/ 里——用 GitHub 的 web search UX** —— 拒：离线可用是 base requirement；`grep` in 本地仓 always works。
7. **写成脚本自动跑 grep 并显示 frontmatter** —— 拒：太定制；grep pattern 对 markdown 已经够简单。

业界共识来源：CPython PEP index 的"search by PEP number / title / topic"的多模式查询设计、Rust RFC 目录的 tracking issues 提供 trail 而非 central index、IETF RFC 对 `rfc-index.txt` 的弱维护（依赖 grep / metadata headers）。这些场景一致证明：plain-text decision log + header-anchored grep 在 100+ 决策规模下比 maintained index 更稳。

**Coverage.**

- 无代码 / 测试 / 构建改动；纯 docs。
- 手工验证每条 grep 命令能真实返回结果：跑 `grep -l 'Rubric §5.1' docs/decisions/*.md` 返回 3+ 文件；`grep -l -F 'Taskflow' docs/decisions/*.md` 返回 3 文件。
- README.md 从 ~53 行 → ~158 行（+105）。现有"模板"、"什么不该进这里"等章节未改。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel / CMake 变更。`docs/decisions/README.md` 一处 append。

**§M 自动化影响.** 本 cycle 是 M2-prep docs debt，不对应任何 M2 exit criterion。§M.1 evidence check 不动打勾状态。§M.2 跳过。
