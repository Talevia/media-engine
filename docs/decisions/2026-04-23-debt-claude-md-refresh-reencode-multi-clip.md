## 2026-04-23 — debt-claude-md-refresh-reencode-multi-clip：删掉已落地的 multi-clip stub 注记（Milestone §M1-debt · Rubric §5.2）

**Context.** `CLAUDE.md:75` 的 Known incomplete 列表里有一条："`me_render_start` re-encode path supports only single-clip h264/aac — multi-clip re-encode is the M1-addendum backlog bullet `reencode-multi-clip` (passthrough already concats N clips)"。但 `reencode-multi-clip` 在 `2bfa6cd` 就已经落地（`ReencodeSegment` struct + `H264AacSink` 拿 N 个 segments + shared encoder 跨 segments + 多 clip end-to-end 回归绿），该 backlog bullet 在同 commit 里删掉了。Known incomplete 陈述的"supports only single-clip h264/aac" 不再是事实——支持的是 N-clip h264/aac concat。

这条过期断言的**直接后果**：读 CLAUDE.md 的新贡献者（包括将来的自己）会被它误导去"实装 multi-clip reencode"——结果发现代码已经在做这件事。正是 `docs/decisions/2026-04-23-debt-claude-md-known-incomplete-refresh.md` 那个 cycle 担心的"narrative-vs-source-of-truth drift"的延续。

**Decision.** 从 `CLAUDE.md` Known incomplete 列表里删除该条。剩下 2 条：

1. `me_render_frame` → M6 frame server（`STUB: frame-server-impl`）
2. `CompositionThumbnailer::thumbnail_png` → M2 compose（`STUB: composition-thumbnail-impl`）

两条都对应 `tools/check_stubs.sh` 输出里的真 STUB markers（本 cycle 验证 `check_stubs.sh` 仍报 3 个 markers：frame-server-impl × 2 + composition-thumbnail-impl × 1——与 CLAUDE.md narrative 1:1 对应）。

**本轮没触碰其他 M1 陈述**（refresh 策略上谨慎）：
- `MILESTONES.md` 的 10 条 M1 exit criteria 仍然未打勾——skill 硬规则 9 禁止自动打勾；下一 bullet `docs-m1-audit` 会产 `docs/M1_AUDIT.md` 作为 user review 材料。
- `docs/PAIN_POINTS.md` 2026-04-22 那条 `reencode-h264-videotoolbox`-related 是痛点 observation，不是 stub 注记——不该在本 refresh scope 内，留给 PAIN_POINTS decay 机制（quarterly audit）。

**Alternatives considered.**

1. **顺手改措辞保留一半**（如"supports only N-clip h264/aac concat; heterogeneous-param segments rejected"）——拒：Known incomplete 的语义是"未实装 / stub / coming later"，不是"phase-1 constraint 列表"。N-clip h264/aac concat 的"identical params required across segments" 约束属于 phase-1 spec，不是 incomplete。硬删更诚实。
2. **Known incomplete 完全删掉**——拒：另两条（me_render_frame、CompositionThumbnailer）确实还是 stub，删 section 会让新读者找不到未实装的 API 清单。只删 entry。
3. **改成 pointer to backlog**（"remaining reencode refinements: see backlog"）——拒：当前 backlog 里没有专门的 "reencode refinement" bullet；剩余工作（例如 sample-accurate trim、跨 codec concat、encoder reuse）是未来 milestone 的 gap，不是当前 incomplete。硬删正确。

业界共识来源：Rust、Kubernetes、Go stdlib 的 README "unimplemented" 列表约定：项目落地即删，不保留"yes we have X now but used to be stubbed"的叙述考古（那属于 git log 和 CHANGELOG 的职责）。

**Coverage.**

- `CLAUDE.md` diff 只删 1 行（Known incomplete 的 reencode-multi-clip 条目）。
- `bash tools/check_stubs.sh` 仍报 3 个 STUB markers（frame-server × 2、composition-thumbnail × 1），和新 CLAUDE.md narrative 2 条自洽（frame-server 一条 narrative 对应两个 call site；composition-thumbnail 对应一个）。
- 无代码 / 构建 / 测试改动。ctest 12/12 不受影响，不重跑。

**License impact.** 无。

**Registration.** 无注册点变更。仅 `CLAUDE.md` 一处编辑。
