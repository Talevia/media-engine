## 2026-04-23 — debt-stub-count-source-unify：stub 计数单一事实源（Milestone §M1-debt · Rubric §5.2）

**Context.** `tools/scan-debt.sh` §2 以前报"C API stubs"数字，跑的是 `grep -c 'return ME_E_UNSUPPORTED' src include` = 11。`tools/check_stubs.sh` 报的是 `STUB:` 标记数 = 3。两个工具同一个概念两个数字，差 8 条——但 8 条是**正常的**"runtime-reject" 路径（"no video stream" / "encoder h264_videotoolbox not available" 之类），`check_stubs.sh` header 里已经 explicit 说过 runtime-reject **不是** stub。repopulate cycle 用 `scan-debt.sh §2` 那个 11 当"stub 数"来判"每个 milestone stub 数下降"，实际在跟踪一个混合指标（真 stubs + runtime rejects）——若新 milestone 加了 5 条 runtime reject、同时关了 3 条 stub，11 → 13，看起来 stubs "涨了"但实际在降。指标飘了。

Bullet 的期望："把 `STUB:` 标记改成唯一事实源；`scan-debt.sh` §2 变一致性检查 raw − marked 应为 0 否则列出未标记行号"。**部分照做**，但"raw − marked = 0 应该"这一条是 bullet 自己理解偏差——`check_stubs.sh` header 已经论证过不该这么算：STUB 的作用是把"没写完的代码"和"写完了只是拒绝某些输入"**结构性分开**。强制等于 0 的话，反而要把所有 runtime reject 都标上 `STUB:`，违背约定。

**Decision.** 重写 `tools/scan-debt.sh` §2，**停止**用 raw ME_E_UNSUPPORTED 当作主指标：

1. **Authoritative stub count** = `grep -rEn 'STUB:[[:space:]]+[A-Za-z0-9_-]+' src` 的行数（当前 3）。和 `check_stubs.sh` 同来源，保证两个工具**报同一个数**。
2. 列出每个 STUB 的 `path:line (STUB: <slug>)`——单 glance 看清每个 stub 是什么（frame-server-impl × 2、composition-thumbnail-impl × 1）。
3. **Raw ME_E_UNSUPPORTED count** 照样跑，但改名叫"Runtime-reject returns (informational)"，数字 = raw − marked（当前 11 − 3 = 8）。明确打标"不是 stub 指标"。
4. 一致性 guard：如果 `raw − marked < 0`（STUB markers 比 ME_E_UNSUPPORTED 返回还多，几乎肯定是 false-positive marker 或代码删了没删 comment），打印 `_inconsistency: STUB markers outnumber raw rejects — investigate_` 的 markdown 告警。
5. 保留 raw-by-file 的分布打印——runtime rejects 的分布在 debt 审视时也值得看（比如 exporter.cpp 突然从 2 涨到 6 可能意味着新增了一批 feature-gating，值得去 review）。

**Alternatives considered.**

1. **完全删 raw count，只报 marked**——拒：跨 cycle 对比"runtime reject 分布变化"也有价值（反映 feature flag / supported codec combo 扩散速度）。保留但改名。
2. **按 bullet 字面要求实现 raw − marked = 0 告警**——拒：会逼着把所有 runtime reject 也打 STUB，污染 marker 含义。`check_stubs.sh` 注释里已经论述了为什么不该这么做。本 decision 里记录这条偏离，让下轮 repopulate 不再把它当 gap 重报。
3. **把 `check_stubs.sh` 合并进 `scan-debt.sh`** 省掉两个脚本——拒：`check_stubs.sh` 输出是 markdown 表格，独立使用（比如贴 PR description），`scan-debt.sh` 是 debt-metric snapshot。两个角色分开。
4. **加"drift 非 0 时必须列出未标记的 `ME_E_UNSUPPORTED` 行号"**——拒：这些行号列表就是"runtime reject list"，已经通过 by-file 分布间接可见；逐行列出 11 条会让 §2 输出膨胀，读者价值低。

业界共识来源：Linux kernel 的 `scripts/check*` 家族（checkpatch、check_exports）都是"authoritative count + informational counts side-by-side" 的模式；Google style guide 的 cpplint 同样区分"real warnings" vs "informational lint"，不把两者混成一个分数。

**Coverage.**

- `bash tools/scan-debt.sh` stdout 的 §2 从"11 条 stubs"变成"3 marked stubs + 8 runtime rejects (informational)"，其他 7 个 section 不动、与上次 snapshot 完全一致。
- `bash tools/check_stubs.sh` 输出不变（仍报 3 条）——两份脚本现在数字对齐。
- 无代码 / 测试改动；ctest 9/9 不受影响，没必要重跑。

**License impact.** 无依赖变更。纯 bash + awk，都是 POSIX 基础工具。

**Registration.** 无 C API / schema / build target 变更。仅 `tools/scan-debt.sh` 一处 §2 逻辑重写。
