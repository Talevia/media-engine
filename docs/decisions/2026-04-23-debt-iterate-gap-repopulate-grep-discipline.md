## 2026-04-23 — debt-iterate-gap-repopulate-grep-discipline：把 grep-verified Gap 变成 SKILL.md 硬性要求（Milestone §M1-debt · Rubric §5.2）

**Context.** 过去一个 session 有**3 次连续** repopulate 失误：bullet 的 `Gap:` 部分凭印象描述"静默丢掉"/"没覆盖"/"未实装"等缺口，结果一 grep 代码才发现缺口早已封住。每次 cycle 都要走完整个"写 plan → 写 decision → 解释为什么 premise 错了"的 motion，一条 bullet 的"做完 or skip"判断耗费比消化 bullet content 还久。三次：

- `debt-render-bitexact-flags` — bullet 说 reencode 非 byte-deterministic；实测早就是 deterministic（FFmpeg ≥ 5.x mov muxer 默认 creation_time=0）。
- `me-timeline-loader-multi-track-reject` — bullet 说 loader 静默接受 multi-track；实际 `timeline_loader.cpp:171` 已有 `require(tracks.size() == 1, ME_E_UNSUPPORTED)`。
- `debt-test-cache-invalidate-coverage` — bullet 说 invalidate 反向语义没覆盖；`tests/test_cache.cpp:116-155` 已经有 "removes entries matching a content hash" case。

模式统一："repopulate 时作者凭印象写 Gap，没用 `grep` 反验代码当前状态"。修复是小的：强制每条 bullet 的 Gap 引用具体 `<path>:<line>` 或 `grep returns empty`。

**Decision.** `.claude/skills/iterate-gap/SKILL.md` 的 §R 在"分档建议"段之后、§R.5 之前插入一段 **Grep-verified Gap（硬性要求，2026-04-23 后）**：

- 每条 bullet 写之前跑 `grep -rn '<relevant_symbol>' src tests include docs`。
- 根据 bullet 类别有 3 条具体模板：
  - "X 没实装" → grep 应返回空或只 `STUB:` marker 行。
  - "Y 没测" → grep 应在 `tests/` 下返回空或只命中不相关 case。
  - "Z 行为可疑" → 直接 Read 文件 + 引用行号到 Gap。
- bullet 的 Gap 必须明写 `<path>:<line>` 或 `grep returns empty`。
- 失败后果（引用本 session 三次实例）：cycle 浪费在补边角。

本 cycle 的 bullet 格式也更新了——自 2026-04-23 `fcf9bce` repopulate 后每个 bullet 都有 `CLAUDE.md:75` / `src/resource/codec_pool.hpp:6` / `tests/...grep returns empty` 等 path:line 证据。这次 SKILL.md 的 amendment 把 "当前实践" 升级成 "硬性要求"。

**Alternatives considered.**

1. **加自动化 checker（pre-commit hook 验证 BACKLOG 里 bullet 的 grep claim）**——拒：脆弱（grep 命令随时间飘）、维护负担大。"SKILL.md 硬性要求 + bullet 作者自我纪律"足够。
2. **把要求塞进 §R.5 内**（把 debt-scan 清单里加一条"bullet 作者自查"）——拒：§R.5 是 debt 扫描细则，"bullet 作者纪律"是更上游的 process 步骤。放 §R（repopulate 总流程）更对位。
3. **把要求放到 `docs/TESTING.md` 或单独 `docs/REPOPULATE_DISCIPLINE.md`**——拒：`iterate-gap` skill 的所有 process 规则都在 SKILL.md 里，跨文件切会让新读者漏看。
4. **不写 amendment，只在人肉记忆里保持纪律**——拒：三次失误已经证明人肉记忆不够。写进 SKILL.md 是承诺。

业界共识来源：Google SWE book §Code Review / Phabricator workflow docs / Kubernetes KEP template — 所有 proposals / bug reports / design docs 都要求"证据链"而非"claim only"；"点击链接能到源代码具体行"是标配。本 amendment 把这条 norm 带进 repopulate 流程。

**Coverage.**

- `SKILL.md` diff 只在 §R 中部（"分档建议" 段之后）插入 ~20 行 markdown，不动其他章节。
- 无代码 / 构建 / 测试改动。`ctest` 12/12 不受影响，不重跑。
- **下次 repopulate 将是第一次正式应用**——本 session 的最后一次 repopulate（`fcf9bce`）已经实操了这条纪律（每 bullet 带 `path:line`），所以 amendment 是把行为正式化。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。仅 `.claude/skills/iterate-gap/SKILL.md` 一处 ~20 行 markdown 插入。
