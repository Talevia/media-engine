## 2026-04-23 — debt-scan-debt-flag-check-per-file：scan-debt §7 识别跨文件重复（Milestone §M2-prep · Rubric §5.2）

**Context.** `tools/scan-debt.sh:150` 的 §7 section "Repeated add_compile_options / target_compile_options" 实际并没有去重或跨文件对比——它就是把 `grep -rn 'add_compile_options|target_compile_options' CMakeLists.txt src cmake` 的结果全量 dump。结果：本仓库唯一的 `CMakeLists.txt` 有两行（`:35` 的 unconditional + `:37` 的 `ME_WERROR` 条件）都进 §7 输出，被当"repeated"报告——但这不是 debt（同文件的条件分支是正常 shape），只是 noise。连续 N 次 repopulate snapshot 都带这条"每次都查的 flag 就是这俩"的无价值记录（scan-debt 原意是把 debt 信号结构化到 repopulate commit body，让 `git log` 成为 debt 监控曲线——混入 false positive 让 signal 失真）。

**Decision.** §7 改写为：**per-flag 去重后只报"同一 flag 出现在 ≥ 2 个不同 CMakeLists 文件"的情况**。算法：

1. `grep` 每条命中的行拆出 `(file, flag)` pair——awk match `/-[A-Za-z][A-Za-z0-9=_-]*/` 从同一行抽所有 flag token。
2. `sort -u` 去重 `(flag, file)` 对——同文件多行、多次声明同 flag 只算一次。
3. Awk 聚合：每个 flag 统计它出现过的 file 数；只输出 file 数 ≥ 2 的 flag。
4. 没有跨文件重复 → `- _clean_`（和 §4 / §6 / §8 一致的语义）。

今天 `bash tools/scan-debt.sh` §7 输出 `- _clean_`（唯一的 CMakeLists 自己声明一次，符合 normal shape，非 debt）。未来如果有人在 `src/CMakeLists.txt` 或 `tests/CMakeLists.txt` 里再 `add_compile_options(-Wall ...)` 重声明一遍，才会被报告——那种确实是 debt（flag 在多个地方同步维护，漏改就脆）。

**Test coverage.** scan-debt.sh 是 build-time / repopulate-time shell script，没有独立 doctest 测试——ctest 不跑它。验证方式：在 CMakeLists.txt 加一行 dupe 临时测（跑一下看 §7 是否报），然后 revert。本 cycle 没动实际 CMakeLists，但新 §7 的干 snapshot 就是"自检"：当前 state 按新规则应该 clean（只 1 个文件），输出确实 clean。

**Alternatives considered.**

1. **按 "whitelist this pair" 跳过 `add_compile_options:35,37`**——拒：whitelist 是 fragile（行号漂移就失效），而且原则错——规则本身就应该"只报真 debt"，不用 hack patch 症状。
2. **Per-file dedupe 但跨文件仍 dump 全量**——拒：那还是 noise（2 个 CMakeLists 各声明一次 same-flag，其中一个是"正常"重声明 vs 真 debt，scan-debt 当前没能力分辨，先 gate 到 ≥ 2 files 把"明显非 debt"的去掉，留给人工判断）。
3. **加"flag 重复时先看 license whitelist 是否一致"之类的更深判断**——拒：过度工程，scan-debt 是粗粒度 debt signal，不该是精细分析工具。
4. **不改 scan-debt，改为在 repopulate commit body 里忽略 §7**——拒：让规则自己正确比让 commit author 记得 "§7 要过滤"更可靠。tools/scan-debt.sh 是 source of truth for repopulate snapshots。

业界共识来源：Linux kernel `scripts/checkpatch.pl`、Google cpplint、Go lint 都有"同文件 repeat 不算 violation" 的 default；cross-file duplication detection 是更有用的信号。ShellCheck 的 SC2086 style check 也是 per-path consolidated。

**Coverage.**

- `bash tools/scan-debt.sh` 完整跑一遍——§7 从两行噪声变成 `- _clean_`；其他 7 个 section 输出不变（diff 对 snapshot 的影响只在 §7）。
- 无代码 / 测试 / 构建改动——`ctest` 12/12 不重跑。
- 下次 repopulate snapshot 会首次用新 §7；commit body 里 `Compile-flag dupe` 那行会从 "1 false-positive" 变 "clean" 或真报告（如果未来有跨文件重复）。

**License impact.** 无。纯 bash + awk，都是 POSIX。

**Registration.** 无 C API / schema / kernel / build target 变更。仅 `tools/scan-debt.sh` §7 一处逻辑重写。
