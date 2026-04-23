## 2026-04-23 — docs-m1-audit：把 M1 exit criteria 的 landed 证据列成表，让用户能 tick 掉 MILESTONES.md（Milestone §M1-debt · Rubric §5.2）

**Context.** `docs/MILESTONES.md` 顶部 `> **Current: M1 — API surface wired**`；M1 的 10 条 exit criteria 里前 3 打勾（engine create/destroy、schema v1 loader、passthrough export），后 7 是 `- [ ]`。但 `git log` 和 `ctest` 都显示这 7 条早已 de-facto 落地（含 `me_probe` / `me_thumbnail_png` / reencode h264+aac / multi-clip loader / doctest + determinism / cache_stats real / 五模块 skeleton）。MILESTONES.md 的 box 状态是 user-owned action 的 artifact：skill 硬规则 9 明确 "Milestone 推进不由本 skill 触发"，不能自动打勾。结果 MILESTONES.md 停在 M1 的时间越久，`iterate-gap` 每 cycle 都因为 milestone hardwall 偏置 M1 任务而阻塞 M2 feature work（transform-static / cross-dissolve / multi-track-compose 等）。

**Decision.** 不打勾、不改 MILESTONES.md 顶部指针——那是用户动作。只产出一份**证据 document** 让用户一次性审阅完就能动：`docs/M1_AUDIT.md`，包含 7 行 markdown 表，每行记：

- **Criterion**：MILESTONES.md 的原文。
- **Landing commit(s)**：从 `git log | grep` 定位的具体 commit shorthash + subject。多 commit 的按时间序列出（例如 reencode 是 `reencode-h264-videotoolbox` → `reencode-multi-clip` → `debt-render-bitexact-flags`）。
- **Test coverage**：哪个 `tests/test_*.cpp` / `examples/0*/main.*` 覆盖，N cases / M assertions 的具体数字。
- **Verified on dev machine**：`ctest` 或 example 的人工跑结果。

`docs/M1_AUDIT.md` 末尾写清 5 步 user 流程：
1. 过一遍表 sanity check 每条 landing commit + test coverage。
2. 觉得 ok 则手工编辑 MILESTONES.md 把 7 个 `- [ ]` → `- [x]`。
3. `> Current: ` 指针改到 `M2 — Multi-track CPU compose + color management`。
4. 写 `docs/decisions/<YYYY-MM-DD>-milestone-advance-m2.md` 记录 milestone 推进事件 + seed M2 backlog（MILESTONES.md 头本身要求"milestone 推进本身也是一次 `docs/decisions/` 记录"）。
5. 同 commit 里删 `docs/M1_AUDIT.md`——这是 one-time 证据 snapshot，不留长期 doc。`git log --follow` 保历史。

**为什么不直接 tick**:

- Skill 硬规则 9 明确禁止。越界会破坏"milestone 推进由用户意图决定"的契约。
- 审判断（"coverage 够不够深"）是 user 的 judgment call：比如 `graph-skeleton` 一条，doctest 覆盖的是 `timeline::segment()` 和 02/03 example 的 stdout，user 可能认为"没覆盖 scheduler 的 parallel path"不算充分。把这份判断留给 user。
- `docs-m1-audit` 这个 bullet 从一开始就 scoped 成 "compile evidence + ask user to decide"——不是 scoped 成 "做 M1 closure"。

**Alternatives considered.**

1. **写进 `CLAUDE.md` 或 `MILESTONES.md` 本身**——拒：audit 是 one-time snapshot，塞进长期 doc 会留"我当时觉得落地了"的考古痕迹；独立 markdown + 用完即删更干净。
2. **不写 doc，在本 decision 里铺开证据表**——拒：decision 文件是"做了什么 + 为什么"的归档，不是"给 user 的待办清单"。角色分离：decision 说做了啥（加 `M1_AUDIT.md`），`M1_AUDIT.md` 说 user 该做啥。
3. **把 MILESTONES.md 的 M1 exit criteria 全删**（"工作都做完了，清单没意义了"）——拒：未打勾的 box 是"未验证"的合约，哪怕 99% 落地，表还提醒未来读者 "确认 user 已同意才推进到 M2"。没 cheaply 解决方式。
4. **加一段 `feat(...)` 的代码让 binary 在 M1 box 全打勾时 banner 升级**——拒：越界，不是 skill 的 scope。

业界共识来源："milestone advance via user-owned checklist + evidence audit" 是 Kubernetes 的 KEP graduation 流程、Go 语言 proposal process、Rust RFC 的 stabilization 走的 pattern——实装者不能自我升级，evidence 由实装者产，review 由其他角色做。

**Coverage.**

- 纯 docs，`docs/M1_AUDIT.md` 新 ≈ 50 行 markdown（表 + 5-step usage note）。
- 每行 landing commit shorthash 都经 `git log --oneline | grep` 二次验证（不是凭印象）——延续上一 cycle 确立的 grep-discipline。
- 每行 test coverage 数字从 `build/tests/test_<name> -s` 的 `[doctest]` 汇总行复制来。
- 无代码 / 构建 / 测试改动，ctest 不受影响。

**License impact.** 无。

**Registration.** 无注册点变更。仅新 `docs/M1_AUDIT.md` 文件 + 本 decision。
