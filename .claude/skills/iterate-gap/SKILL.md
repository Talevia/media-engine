---
name: iterate-gap
description: 从 docs/BACKLOG.md 挑当前 milestone 最高优先级任务，plan → 实现 → commit（理由写进 commit body）→ 推 main。backlog 空了再按 rubric + milestone 一次性补 15 条。参数 "<count> [parallel]"，例：/iterate-gap、/iterate-gap 3、/iterate-gap 3 parallel。执行期间零提问。
---

# iterate-gap — milestone-aware backlog-driven 补齐愿景 gap 的循环

挑当前仓库与北极星之间**当前 milestone 内**优先级最高的 gap，plan → 在 `main` 上实现 → commit → 推送。**执行期间不向用户提任何问题**——每一个决策都按 `docs/VISION.md` + `docs/MILESTONES.md` + 业界共识自主做出，理由写进 commit message body（见 §7），由用户事后通过 `git log` 异步审阅。

**任务源是 `docs/BACKLOG.md`**（P0 → P1 → P2，同档内按出现顺序取第一个）。只有当 backlog 被清空时，才 fallback 到 rubric + milestone 分析，一次性生成 15 条新任务写回 backlog，并在同一 cycle 里继续挑新生成的第 1 条动手。

## 参数

按 `<count> [parallel]` 解析：

- 无参数 → `count=1`，顺序执行
- `<N>`（如 `3`）→ `count=N`，顺序执行（一个 cycle 做完再做下一个，每个开始前 rebase 一次）
- `<N> parallel`（如 `3 parallel`）→ `count=N`，用 git worktree 并行，等全部跑完后依次合回 `main`

上限：顺序模式 `count ∈ [1, 6]`，并行模式 `count ∈ [2, 3]`（C++ 改动通常比 Kotlin 跨文件更广，并发度放低一点）。参数格式有误时**默认退回 `1` 顺序执行**，不问用户澄清。

## 操作约束（两种模式都适用）

- **分支目标**：`main`。不开 feature branch，不开 PR。（并行模式内部用临时 worktree 分支，但本次调用内部必须通过 merge+push 落回 `main`。）
- **提问**：零次。遇到决策点按 VISION + MILESTONES + 业界共识自行决定，理由写进 commit message body（见 §7）。真卡在只有用户能回答的问题（付费授权、私钥、产品偏好）→ **换一个 gap**（从 backlog 下一条取），不干等，也不问。
- **Backlog 是唯一任务源**：不凭空脑补"临时 gap"绕过 backlog。空 backlog → 先 rubric-repopulate（见 §R），再继续。
- **Milestone 硬闸**：当前 milestone（`docs/MILESTONES.md` 顶部 "Current: " 指针）的 exit criteria 未全打勾时，不处理下一个 milestone 的 gap——除非所有当前 milestone 的 backlog 项都被跳过（踩红线 / 缺用户输入）。**milestone 推进由本 skill 在每个 cycle 的 step 7 后自动处理**（见 §M "Milestone sync"）：evidence-complete 的 exit criterion 自动打勾（独立 `docs(milestone):` commit），全部打勾后自动推进 "Current:" 指针到下一 milestone 并 seed bootstrap backlog。evidence 不足的 criterion 不动，下 cycle 再审。
- **先 plan 再实现**：每个 gap 必须有独立 plan 步骤之后再动代码。
- **决策理由落在 commit body**：每轮**一个** `feat(...)` commit（或 `fix` / `refactor`）同时包含：代码改动 + `docs/BACKLOG.md` 对应 bullet 的删除；**决策理由写进 commit message body**（context / decision / alternatives / coverage / license / §3a self-check / registration / §M 影响，见 §7）。`git log` 即归档。

---

## 顺序模式（默认）

重复 `count` 次。每次迭代之间 `git pull --rebase origin main`。如果当前 milestone 下 backlog 空、rubric 也找不出有效 gap（当前 milestone 所有 exit criteria 已达成）→ **提前停止**，不凑数。最后报告诚实写 `完成 N / 请求 M / 当前 milestone 已满足`。

### 1. 同步 main

```
git fetch origin
git pull --rebase origin main
```

工作树脏或 rebase 失败 → **停下报告**，不丢弃修改。

### 2. 读 backlog + milestone，挑一个

读 `docs/MILESTONES.md` 顶部记录 current milestone（例如 `M1`）。

读 `docs/BACKLOG.md`。分情况：

- **P0 有未完成项** → 挑 P0 最靠上一条。优先选 `Milestone §M<current>` 的；若 P0 全是非当前 milestone，先选 P0 里的（P0 总是压过 milestone 偏置）。
- **P0 空、P1 有当前 milestone 项** → 挑当前 milestone 的第一条 P1。
- **P0 空、P1 无当前 milestone 项但 P2 有** → 仍优先 P1 第一条（跨 milestone 的 debt 优先于 P2 的未来 milestone 项）。
- **P0 / P1 都空** → 跳到 §R 做 repopulate。

挑出后记录：slug、Gap / 方向 / Milestone / Rubric 轴原文；同档下一条（亚军）留给最后报告。

**不再**做"自由式 rubric 分析 + 重排"；backlog 已把优先级写死。

### R. Backlog repopulate（只在 backlog 空时执行）

触发：第 2 步命中 "**P0 / P1 都空**" 分支（或 `docs/BACKLOG.md` 文件不存在）。**P2 的存在性不影响触发**——P2 是下一 milestone 的储备区 + 未来 milestone feature 起步项，不充当当前 milestone 的 fallback。P0/P1 都空意味着"当前 milestone 没待办活儿了"，此时要么用 rubric 找补、要么准备 milestone advance（§M.2 全绿情况下会在上一 cycle 触发）。

依次读：

1. `docs/VISION.md` §5 的 7 条 rubric 轴（5.1–5.7）。
2. `docs/MILESTONES.md` 顶部的 current milestone 及其未打勾的 exit criteria。
3. `CLAUDE.md` 的 "Architecture invariants" + "Anti-requirements" + "Known incomplete"——已承认的非回归项别当缺口重复报。
4. 最近 ~30 个 commit 的 message body（`git log -30 --format='%h %s%n%b%n---'`）——近期决策理由已约束了不该再做一遍的内容。
5. `git log --oneline -30`——看最近落地了什么。

走读 `include/media_engine/`、`src/timeline/`、`src/api/`、`src/io/`、`src/render/`（将来）、`src/audio/`（将来），对每条 rubric 轴打分（有 / 部分 / 无），**以当前 milestone 的 exit criteria 为优先补口**。

产出**恰好 15 条**新任务，硬性排档规则：

1. **Milestone 优先级**：
   - 当前 milestone 未打勾的 exit criteria → **P0 / P1**（按 severity）。
   - 跨 milestone debt（上一 milestone leftover polish、跨多 milestone 的文件级 debt、`debt-*` 类观察）→ **P1 可接受**；severity 低的归 P2。这对应 Step 2 的"跨 milestone 的 debt 优先于 P2 的未来 milestone 项"——repopulate 时保留这类 bullet 在 P1 是合法的。
   - 下一个 milestone（§M<current+1>）的 feature 起步项 → **P2**。
   - 再下一个 milestone（§M<current+2>）及以后 → **不 seed**，真到那时再说。
2. **一等抽象 > patch** —— 可复用抽象（新 effect 注册 API、新 I/O 抽象层）归 P0 / P1；单一用途的 patch 归 P2 或删。
3. **短周期能闭环** —— 同档内能一个 cycle 内闭环的排前面。

**Debt 占比硬性要求**：15 条里**至少 4 条（~30%）**是 `debt-*` 任务，来自下面 §R.5。debt 扫描不到 4 条 → 如实填（宁可 feature gap 减少也不砍 debt 名额）。

分档建议：P0 约 3 条（含 1 条 debt）、P1 约 7-8 条（含 2-3 条 debt）、P2 剩余。找不满 15 条可少（最少 6 条，其中 debt ≥ 2）。

**Grep-verified Gap（硬性要求，2026-04-23 后）**：每条 bullet 的 `Gap:` 部分**必须引用具体 `path:line` 证据**，不凭印象写"静默丢掉"/"没覆盖"/"未实装"等转述。写 bullet 前跑：

```
grep -rn '<relevant_symbol>' src tests include docs
```

验证 bullet 描述的 gap **真的存在**：

- 声称 "X 没实装" → `grep -n 'return ME_E_UNSUPPORTED\|STUB:' src/<module>/<file>.cpp` 或 `grep -n '^void X\|^me_status_t X' src/` 必须空或只返回 stub marker。
- 声称 "Y 没测" → `grep -rn 'Y\|<slug>' tests/` 必须空或只命中不相关 case。
- 声称 "Z 行为可疑" → 直接 `Read` 相关文件 + 引用具体行号到 bullet 的 Gap 里。

写 bullet 时在 Gap 部分明写 `<path>:<line>` 或 `grep returns empty`；这样未来 cycle 选到这条 bullet 时看到证据链能直接 Read 对应位置 sanity-check，不用自己重新找。**不按这条做的后果**：bullet premise 和代码不一致，cycle 浪费在"补边角"或纯粹 skip（2026-04-23 session 有过 3 次连续失误：`debt-render-bitexact-flags` / `me-timeline-loader-multi-track-reject` / `debt-test-cache-invalidate-coverage`，每次都耗半个 cycle 搞清楚 bullet 错在哪——比打字几行 `path:line` 贵几十倍）。

### R.5 技术债扫描（repopulate 必做）

**先跑 `bash tools/scan-debt.sh`**——这份脚本把下面 8 类信号固化成结构化输出，避免每次 repopulate 重新手搓 grep 命令造成结果漂移。把脚本的 stdout 完整粘到 `docs(backlog)` commit message body 里；下面的"阈值 → debt 任务"映射仍由本节规约：

每类命中产一条 `debt-` 任务。信号与上次 `docs(backlog)` commit 所在的快照对比：

1. **长文件** —
   ```
   find src include -name '*.cpp' -o -name '*.hpp' -o -name '*.h' | xargs wc -l | sort -rn | head -10
   ```
   > 400 行自动入 debt 候选 slug `debt-split-<filename>`。> 700 行强制 P0 / P1。C++ 头比 Kotlin 文件倾向更密集。

2. **C API stub 蔓延** —
   ```
   grep -rn 'return ME_E_UNSUPPORTED' src/api src/core
   ```
   和上次快照对比。数量不减反增 → 一条 `debt-stub-regression`。C API 的目标是**每个 milestone stub 数下降**，不能有新增。

3. **TODO / FIXME / HACK / XXX 累积** —
   ```
   grep -rnE 'TODO|FIXME|HACK|XXX' src include cmake | wc -l
   ```
   净增长 > 0 出一条 `debt-clean-todos`，commit body 里列新增行号。

4. **被跳过或禁用的测试** —
   ```
   grep -rnE 'SKIP|DISABLED|SUBCASE.*skip|GTEST_SKIP|DOCTEST_SKIP' tests 2>/dev/null
   ```
   每个 skip 出一条 `debt-unskip-<test-name>`（要么修要么删）。

5. **重复依赖 / hand-roll 替代** —
   ```
   grep -rn 'FetchContent_Declare\|find_package' CMakeLists.txt src cmake
   ```
   vs `grep -rn 'std::unordered_map\|std::string\|memcpy' src`——如果有代码自己实现 JSON 解析 / UTF-8 / sha256 / base64 而这些在白名单依赖或标准库里已有，出一条 `debt-replace-<name>`。

6. **被标记 `[[deprecated]]` 或头文件注释 "DEPRECATED" 的 API** —
   ```
   grep -rn '\[\[deprecated\]\]\|DEPRECATED' include src
   ```
   存在超过 1 轮 repopulate 周期未清理 → 一条 `debt-remove-deprecated-<symbol>`。

7. **CMake 构建选项 / flag 冗余** —
   ```
   grep -rn 'add_compile_options\|target_compile_options' CMakeLists.txt src
   ```
   同一个 flag 在多处重复声明 → 一条 `debt-centralize-flags`。

8. **头依赖传递不干净**（`include/` 里的公共头 `#include` 了非白名单头）—
   ```
   grep -rn '#include' include/
   ```
   公共头只能 include `<stddef.h> <stdint.h>` 及相邻公共头——出现任何其他标准库 / 第三方头是硬违规，出一条 `debt-clean-public-includes`。

扫描产出的 debt 按严重度入档：

- **强制 P0**：长文件 ≥ 700、stub 数净增长、公共头含不该含的依赖。
- **默认 P1**：长文件 400–700、被跳过的测试、hand-roll 替代、flag 冗余。
- **默认 P2**：TODO 净增长、单个 deprecated。

扫描结果写进 `docs(backlog)` commit message body（简洁列出各指标对比数字），`git log` 本身就是劣化监控曲线。

写入 `docs/BACKLOG.md` 的 bullet 格式与现有文件一致：

```
- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。
```

**Repopulate 本身是独立 commit**（只动 `docs/BACKLOG.md`，不带代码）：

```
docs(backlog): repopulate 15 tasks from rubric + milestone analysis

Milestone: M<current>
Metrics vs previous snapshot:
- long files > 400 lines: N → M
- API stubs returning ME_E_UNSUPPORTED: N → M
- TODO/FIXME count: N → M
...
```

只改 `docs/BACKLOG.md` 一个文件。push 之后**不把 repopulate 本身算作本轮 "1 个 gap"**，继续回到第 2 步挑新列表第 1 条，走完整 plan → 实现 → 删 bullet → 推送的 cycle。

### 3. Plan

内部 plan（不要用 ExitPlanMode——零提问）。Plan 必须包含：

- 对应的 milestone + rubric 轴（复制 backlog bullet 里标的 `§Mx` 和 `§5.y`）。
- 会改 / 新增 / 删除的文件清单。
- 这次改动必须守住的 CLAUDE.md 架构规则（C API `extern "C"`、无 GPL 依赖、有理数时间、不透明 handle、确定性保证）。
- 跑哪些验证：`cmake --build build` + 新老 smoke example + 相关 ctest + `-Werror` 构建。
- **反需求核查** —— 扫一遍 CLAUDE.md "Anti-requirements"。踩线 → **丢掉 plan 换下一条 backlog**。
- **设计约束自查** —— 过一遍 §3a 清单。任意一条命中 → **换下一条**。

如果 plan 需要只有用户能给的信息（付费 license 密钥、特定硬件访问、品牌偏好）→ **换下一条 backlog bullet**。被跳过的 bullet 保留不动。

### 3a. 设计约束自查清单（Plan 必跑）

每条命中"是 / 可能"→ 换 backlog。历史上系统劣化几乎全部来自连续的"一次例外"。

1. **类型化 effect 参数不回退** — 本轮会引入 `std::map<string, float>` / `std::unordered_map<string, float>` 形状的 effect 参数 API 吗？必须是 typed struct / variant / tagged union。退回 map 是 VISION §3.2 明确禁止的。

2. **时间不用浮点** — 本轮代码里出现 `double seconds` / `float seconds` / `int64_t milliseconds` 了吗？必须 `me_rational_t`。转换函数如 `me_rational_to_seconds_double` 只能出现在 log / debug / UI 边界，不能进入决策逻辑。

3. **公共头不含私有依赖** — 本轮会往 `include/media_engine/*.h` 里加 `#include` 吗？允许 `<stddef.h> <stdint.h>` 以及相邻公共头。出现 `<string>`、`<vector>`、FFmpeg、nlohmann、bgfx、Skia 任何一个 → 红线。

4. **C API 不泄露异常 / STL** — `extern "C"` 函数里有 C++ 异常可能逸出？有 `std::string` / `std::vector` 作为参数或返回类型？必须 catch-all 翻译成 `me_status_t` + last_error，POD 跨边界。

5. **GPL 不混入** — 本轮 CMake 是否新增 `FetchContent_Declare` / `find_package`？license 是否在 ARCHITECTURE.md 白名单里？libx264 / libx265 / Rubberband-GPL / Movit 直接阻断。

6. **确定性不破坏** — 本轮引入了：parallelism？SIMD FMA？`std::unordered_map`（iteration order 不定）？`rand()`？时间戳入日志路径但也入输出？任何一条 → 在 commit body 里显式说明是否破坏软件路径 byte-identical，或者如何证明没破坏。

7. **Stub 不净增** — 本轮添加新的 `ME_E_UNSUPPORTED` 返回点吗？只允许两种情况：(a) 新加的 API 函数暂时 stub 但必须同 commit 在 BACKLOG 里加对应 `*-impl` bullet；(b) 实装的新路径把**更多**旧 stub 消除掉。净 +1 stub 不带 backlog bullet → 红线。

8. **OpenGL 不进主路径** — 本轮引入 GL API 吗？必须明确标注 fallback 路径（文件名 `*_gl_fallback.*` / namespace / CMake option 守护）。bgfx 不算（它是抽象层）。

9. **Schema 向后兼容** — 改动 JSON schema 吗？新字段必须有默认值（旧 JSON 能解）。删字段前 `git log --all -p -- 'docs/**.md' 'src/timeline/**'` 或 `git log -S '<field>'` 看是否有依赖。破坏性变动 → 必须 bump `schemaVersion`。

10. **ABI 不破坏** — 改动 `include/media_engine/*.h`？开过的 enum 值不改、开过的 struct 不往中间插字段（只允许末尾 append）、开过的函数签名不动（需要时加 `me_foo2`）。ABI 破坏性变动 → 必须在 commit body 里说明为什么接受 + 更新 `docs/ARCHITECTURE.md` 的当前实现状态表。

**命中任何一条不是"想办法绕过"的信号，是"换下一条 backlog"的信号**。

### 4. 实现

- C++20，`-Wall -Wextra -Werror -Wno-unused-parameter` clean。
- `nlohmann::json` 走 `JSON_DIAGNOSTICS=0` 路径（不吃 RTTI / 异常传到 C 边界）。
- 新 public header 必须通过 C-only compile check（`clang -xc -std=c11 -fsyntax-only`）。
- 新 effect kind / codec / container 注册时，必须带类型化参数 schema。
- FFmpeg 调用走 RAII wrapper（见 `src/io/ffmpeg_remux.cpp` 的模式）或 `unique_ptr` + 自定义 deleter。

### 5. 验证

按改动区域跑最贴的 target：

| 改动区域 | 最低验证 |
|---|---|
| `include/` 公共头 | `clang -xc -std=c11 -fsyntax-only -Iinclude -` 喂一个最小 C 用例；然后 `cmake --build build`。 |
| `src/api/**` | `cmake --build build` + 跑 `01_passthrough` 回归（现有 sample.timeline.json）。 |
| `src/timeline/**` | 同上 + 跑负面 schema 测试（schemaVersion=2 / 带 effects）。 |
| `src/io/**` | `01_passthrough` 端到端 + ffprobe 检查输出（container / codec / duration / 帧数）。 |
| `src/render/**`（future） | 确定性测试：同 JSON 渲染两次字节对比。 |
| `CMakeLists.txt` / `cmake/` | 干净 `rm -rf build && cmake -B build -S . && cmake --build build`，确认 FetchContent 下载 + 链接都过。 |

**必跑**：完整 `cmake --build build --target 01_passthrough` 后生成的 binary 能跑通现有 sample timeline，ffprobe 产物合法。**红色不准 commit**。

### 6. 删 backlog bullet（+ 可选：顺手记 debt）

把本轮处理的 bullet 从 `docs/BACKLOG.md` 删掉：

- 只删这一条 bullet（整行含前导 `- `），**不**重写整个文件、不动顶部说明、不 reorder 剩余 bullet。
- 被跳过的 bullet 保留不动（plan 阶段判定踩红线 / 缺用户输入的那种）。
- `git diff docs/BACKLOG.md` 确认只减不增——**除了**下面这种情况：

**顺手记 debt**（本 cycle 唯一允许的 bullet 新增）：实现 / 验证过程发现一条与本任务无关的技术债（某文件突然长到 500 行、FFmpeg 调用发现一个 leak 路径、撞上两个近似函数），**不要修**（会把本 commit 偏离 plan），而是在 `docs/BACKLOG.md` P2 档末尾 append：

```
- **debt-<slug>** — <发现了什么>。**方向：** <建议修法>。Milestone §Mx / Rubric 外·顺手记录。
```

规则：
- 只能 append 到 P2 末尾，不能插 P0 / P1，不 reorder。
- 一次 cycle 最多 append 2 条（更多说明跑偏了）。
- 和本轮处理的 bullet 删除 + 代码改动一起进同一条 `feat(...)` commit，不单独 commit。
- **只记不修**——"观察笔记"，下次 repopulate 或专门调度时处理。

**观察分流（BACKLOG 还是 PAIN_POINTS）**：上面的 debt append 是**默认路径**，几乎所有 cycle 级偶然发现都走这条——代码异味、重复模式、某个文件变长、helper 抽取时机未到，都记 BACKLOG。唯一例外是观察同时满足 `docs/PAIN_POINTS.md` 头部三条准入门槛：

1. 痛感追溯到一条具名硬规则（VISION 公理 / CLAUDE.md anti-requirement 或架构不变量 / ARCHITECTURE.md ABI 承诺），不是"抽象没到位"。
2. 成本随新 codec / effect / asset 类型**递增**，不是一次性。
3. 方案需要**修改或重新权衡规则**，而不是只加一个 helper。

三条全中 → append 到 `docs/PAIN_POINTS.md` 末尾（格式见该文件 header），**不走 BACKLOG**。任意一条不确定 → 默认 BACKLOG。一个 cycle 通常 PAIN_POINTS 条目 = 0，最多 1；如果一周内 ≥ 2 条往这边走，说明分流标准放松了，下个 cycle 先审视。PAIN_POINTS 条目和 debt bullet 一样进同一条 `feat(...)` commit，不单独 commit。

### 7. Commit + push

- 按具体文件名 stage（严禁 `git add -A`，CLAUDE.md 硬规则）。
- 一条 commit：`feat(<area>): <短标题>`（或 `fix` / `refactor`），包含两件事**一起**提交：
  1. 代码 / 测试改动。
  2. `docs/BACKLOG.md` 里对应 bullet 的删除（+ 可选 debt append 或 PAIN_POINTS 条目）。
- **决策理由写进 commit message body**（以前是 `docs/decisions/<date>-<slug>.md` 单独文件，现在并入 commit body；`git log` 即归档，`git log -S '<symbol>'` / `git log --grep=<slug>` 查旧决策）。body 至少包含以下段落（用 `HEREDOC` 传入避免手写换行出错，参考 CLAUDE.md 里的示例）：

  - **Context.** 本轮处理的 backlog bullet（slug + 一句话 gap 复述 + milestone §Mx + rubric §5.y）。grep 证据为 `path:line` 引用上游 gap。
  - **Decision.** 落地了什么。关键类 / 函数 / 文件名。
  - **Alternatives considered.** 至少 2 条被拒的方案 + 为什么拒。业界共识论据（FFmpeg / libav / bgfx / OCIO 等常见做法）要具体到名字。
  - **Coverage.** 哪些 test / example 覆盖了本轮改动。命中的 ctest suite + 新增 TEST_CASE 数。
  - **License impact.** 新依赖？在 ARCHITECTURE.md 白名单里？纯代码改动写 "无新依赖"。
  - **§3a self-check.** 10 条命中情况。全绿简记 "全绿"；任意一条有命中必须标出并说明为什么能例外（或为什么不是真命中）。
  - **Registration.** 改了哪些注册点 / 入口（TaskKindId / CodecPool / Orchestrator factory / C API 函数 / CMake / JSON schema 字段），或写 "无注册点变更"。
  - **§M 自动化影响.** 本 cycle 是否 tick 某个 milestone exit criterion？推进了 milestone？或者 "M3 exit criteria 不变"。

  Body 长度按需——简单 bug fix 半屏足够；重大架构改动可多屏。**不写 commit hash**（自引用；`git log` 本身就是 canonical）。

- `git push origin main`。
- 推送被拒（有人同时推过）→ `git pull --rebase origin main` → rebase 动了文件就再跑一遍验证 → 再推。rebase 冲突解不开 → **停下报告**。绝不 `--force`，绝不对已推送 commit `--amend`。

Backlog repopulate 是例外：独立一条 `docs(backlog): …` commit（见 §R），只改 `docs/BACKLOG.md`，不涉及代码；body 只需列 debt 扫描指标对比（前→现数字）。

### M. Milestone sync（step 7 之后、step 8 之前必跑）

目的：让 "Current: Mx" 指针永远反映**实际**仓库状态，而不是过去某次人工 snapshot。两步：

#### M.1 Evidence 审核 + 打勾

读 `docs/MILESTONES.md` current milestone 的所有 `- [ ]` 未打勾行。对每条 criterion：

1. 用 grep 找出关键 API symbol / concept 名（e.g. `me_probe`、`me_thumbnail_png`、`OCIO`、`cross-dissolve`）。
2. Evidence 三元组（必须**全部**满足才算"done"）：
   - `src/` 里该 symbol 非 stub 实装存在（grep: 相关函数体不 `return ME_E_UNSUPPORTED;` 不挂 `// STUB:` 标记）。
   - CI 覆盖存在（`tests/test_<symbol>*.cpp` 有对应 TEST_CASE，或 `examples/0*/` 端到端覆盖）。
   - 最近 ~30 commits 里有承诺实装该 symbol 的 `feat(...):` commit（`git log --oneline -30 | grep -i '<symbol>'`）。
3. 三条 evidence 都命中 → 可打勾。任一条缺 → 保留未打勾，不写日志，下 cycle 再审。

**本 cycle 可打勾的 criterion 集合到一起**：

- 集合 = 0 条 → MILESTONES.md 不动，跳到 M.2（可能已全绿，准备推进）。
- 集合 ≥ 1 条 → 把对应 `- [ ]` 改成 `- [x]`，独立 `docs(milestone): tick exit criteria <逗号分隔 slug>` commit（只动 `docs/MILESTONES.md`），push。

#### M.2 全绿 → 推进 milestone

M.1 之后若 current milestone 所有 exit criteria 都已打勾：

1. 挑下一个 milestone：MILESTONES.md 文件内下一个 `## M<N> — ...` 章节。
2. 改文件顶部 "Current: " 指针指向新 milestone 标题原文。
3. Seed bootstrap backlog bullets（写回 `docs/BACKLOG.md`）：
   - 已存在于 P2 档且 `Milestone §M<new>` 标签的项 → 整条**移动**到 P1 档末尾（保留 slug / Gap / 方向文字，只改档位）。
   - 若新 milestone 的 exit criteria 里有**任何一条**目前没有任何 BACKLOG bullet 对应 → 写 ≥ 1、≤ 5 条新 P1 bullet 覆盖这些缺口。每条按 BACKLOG 标准格式 `- **<slug>** — <Gap>。**方向：** ...`，**Gap 部分必须引用 grep 到的 path:line 证据**（SKILL §R 硬规矩）。
4. 如仓库有 `docs/M<prev>_AUDIT.md`（一次性 evidence snapshot）→ 同 commit 删除，`git log --follow` 已保留历史。
5. 单一 `docs(milestone): advance Current to <new-milestone-slug> — all <prev> exit criteria landed` commit，包含 MILESTONES.md + BACKLOG.md + audit 文件删除（若有）。**Commit body 承载 milestone-advance 决策**：上 milestone 所有 exit criterion 的 landing commit hash（`git log --oneline` 摘一下）+ 测试覆盖清单 + 下 milestone bootstrap bullet 选择理由。push。

推进后，**本 cycle 就此结束**（不再挑新 backlog 任务，milestone-advance 本身就是本 cycle 的产出）。报告里明标 "milestone advanced to <new>"。下次 `/iterate-gap` 正常 pick top P1 就自动是新 milestone 的工作。

#### M.3 推进后 new milestone 的 backlog 稀薄

M.2 之后常见情况：P2 里已存在的下一 milestone 储备项被 promote 到 P1 + 新 seed 1–5 条覆盖未覆盖的 exit criteria，total ~3–6 bullet。少于 3 条也不强制同 cycle 补仓：

- **M.2 本 cycle 不另触发 §R repopulate**（避免 milestone-advance 变三连 commit）。
- 后续 `/iterate-gap` 按 Step 2 正常挑 P1。每挑一个，剩余 P1 变少。等某次挑完**最后一个 P1** 时 P0/P1 都空——Step 2 **自然**命中 §R 触发条件 → 那 cycle 走 repopulate。
- Step 2 **不**检测 "P0+P1 < 3"；只检测"空"。M.3 的实际含义是"放心让 backlog 自然耗尽到空再补"，不是"提前触发"。

#### M.4 何时可以跳过整个 §M

- 本 cycle 就是 `docs(backlog):` repopulate（§R）→ 跳过 §M（repopulate 不改 code，不推进实装）。
- 本 cycle 是 milestone-advance 本身（上一 cycle 的 M.2 产出）→ 已经在 §M.2 里做完，不重复跑。

### 8. 继续或收尾

还有剩余 iteration → 回第 1 步。否则输出报告：

- 本次处理的 gap（每个一行：milestone + rubric 轴 + slug + 摘要）。
- 推送的 commit（shorthash；如果本轮触发了 repopulate，也列 `docs(backlog)` 那条；**如果 §M 触发了 tick / advance，也列对应 `docs(milestone)` commit**）。
- 跑了哪些验证 + 结果。
- 实现过程中意料之外的事。
- 本轮是否触发了 backlog repopulate；当前 backlog 剩余各档条数（P0 / P1 / P2）。
- **Milestone sync 结果**：本 cycle ticked 了哪些 exit criteria（slug 列表）；是否推进到下一 milestone；当前 milestone 剩余未打勾数。
- 下次可挑的候选（backlog 新的 top-1），供用户决定是否再次触发。

---

## 并行模式（`<N> parallel`，N ∈ [2, 3]）

在隔离的 git worktree 里并发跑 N 个 cycle，跑完后在**本次调用内部**依次合回 `main`。

### P1. 同步 + 读 backlog + milestone + 选出 N 个「互不重叠」的 gap

同顺序模式第 1、2 步（`git fetch` + rebase + 读 MILESTONES + 读 BACKLOG）。

从头往下扫（P0 → P1 → P2，同档内按出现顺序 + milestone 偏置），挑 N 个**互不重叠**的 bullet：

- **互不重叠**的定义：两个 gap 预期改动的文件集合两两不相交。主调度器为每个候选 bullet 预判改动文件集（依据"方向"文字 + 走一遍相关源码），两两核查不相交。冲突时保留更靠上的那条（优先级高）。
- 互不重叠的数量 < N → **静默缩减 N** 到最大不重叠子集，最后报告提到缩减。
- 只有 1 个不重叠 bullet → **剩余名额 fallback 到顺序模式**。
- Backlog 空（P0/P1/P2 全清）→ 先顺序模式做一次 §R repopulate（不能并行做 repopulate，文件会冲突），commit + push，再回到本步读刚生成的 backlog。

### P2. 派发 N 个并行 agent

用 Agent 工具调用，`isolation: "worktree"`，**所有 N 次调用放在同一条消息里**，确保并发。

每个 sub-agent 的 prompt 自包含。必须包含：

1. 分配给它的具体 backlog bullet（完整原文：slug / Gap / 方向 / milestone / rubric 轴）+ P1 plan 里给出的预期改动文件清单 + 当前 MILESTONES 里该 milestone 的 exit criteria 原文。
2. 复制顺序模式第 3–7 步（plan → 实现 → 验证 → 删 bullet → commit，决策理由写进 commit body），仅一处调整：
   - **分支**：在 worktree 自动创建的分支上 commit——不改名、不 checkout `main`、**不要 push**。分支保持未推送状态，由主调度器合。
   - `docs/BACKLOG.md` 只删**自己这条** bullet。各 sub-agent 删的是不同行，git merge 会当独立 hunk 合并，不冲突；但**不得** reorder 剩余 bullet。
3. 它必须跑的验证（第 5 步的表）。
4. 输出契约：最终消息必须包含 commit SHA、删除的 bullet slug、一句话结果摘要、验证是否绿。失败不 commit，把错误返回。

并行度上限 3。N > 3 静默 clamp。

### P3. 收集结果

分流：

- **成功**（有 commit，验证绿）→ 合并队列。
- **失败**（没 commit，或验证红）→ 丢掉这个 worktree 分支，不合，记入最终报告。

零成功 → 报告后停，不要 fallback 到顺序。

### P4. 依序合回 main —— 每个分支合完立刻 push

对每个成功分支，按确定性顺序（milestone 优先 + 时间戳），**合完一个 push 一个**：

1. `git checkout main`
2. `git pull --rebase origin main`
3. `git rebase main <branch>` —— 代码冲突在这里解决。
4. Fast-forward 合入 main：`git checkout main && git merge --ff-only <branch>`。
5. **立刻 `git push origin main`**。
6. 冲突处理：
   - `docs/BACKLOG.md`：预期不冲突（每个 sub-agent 删不同行）。真冲突 → 基于 `main` 当前状态重建"只删这一条 bullet"的 diff 再 merge。不能丢别的 sub-agent 的删除。
   - 代码冲突 → **停下报告**，剩余分支保留原状让用户检查。
7. 推送被拒 → `git pull --rebase origin main` → 再推。解不开 → **停下报告**。
8. 回第 1 步处理下一个分支，直到队列空。

### P5. 清理 / 报告

- 已合并的 worktree 分支删。
- 失败 / 中止合并 → **保留 worktree + 分支**让用户手动检查，路径和分支名写进报告。
- 报告：请求 N，派发 M（不重叠过滤后），成功 K，合入 main K'；推送 commit shorthash；跳过 / 失败 gap + 原因；遗留 worktree + 分支（给路径）；下次可挑的亚军候选。

---

## 硬规则（两种模式都不准违反）

1. **一个 cycle 内零提问**。卡住 → 换 backlog 下一条或停。
2. 最终状态落在 `main`。并行模式的中间分支要么合入、要么作为遗留项报告，绝不静默丢弃。
3. **Commit → push 永远配对**。本地 `main` 一出现新 commit → **立刻** `git push origin main` 再开下一个 cycle / 合并。绝不让本次调用结束时本地 `main` 有未推送 commit。
4. 一条 `feat(...)` commit 同时包含代码 + BACKLOG bullet 删除（允许末尾 append 最多 2 条 `debt-*`，或 ≤ 1 条 `PAIN_POINTS.md` 条目，见 §6 "观察分流"）。**决策理由写进 commit message body**（见 §7 的 body 段落约定）。repopulate 的 `docs(backlog)` commit 例外（只改 BACKLOG，不带代码）。
5. 绝不 `--no-verify`、绝不 `--force`、已推送 commit 绝不 `--amend`、绝不 `git add -A`。
6. 绝不绕过 CLAUDE.md 架构规则或反需求清单。bullet 必须绕才能做 → **跳过它取下一条**（bullet 原样保留给用户裁决）。
7. **设计约束 §3a 10 条是硬性否决**。任意一条命中"是 / 可能"→ 换 backlog 下一条。不允许"这次就例外一下"。
8. **Backlog 是权威任务源**。不凭空发明临时任务，不跳过 P0 直接做 P2。空 backlog → repopulate，不绕过。
9. **Milestone 推进由本 skill 自动处理**——每个 cycle 的 step 7 之后必跑 §M "Milestone sync"：evidence-complete 的 exit criterion 自动打勾（独立 `docs(milestone):` commit）；全绿后自动推进 "Current:" 指针并 seed 下一 milestone bootstrap backlog。Evidence 不足 → 不打勾，保留原状下次 cycle 再审。本 skill 仍然在偏置任务时优先挑当前 milestone 的 backlog 项。
10. **Repopulate 必须 ≥ 30% debt 任务**（详见 §R.5）。跳过 debt 扫描 / debt 占比不足的 repopulate commit 不合法，下一 cycle 发现立刻回滚该 repopulate 并重做。
11. 并行模式并发度 ≤ 3。超了静默 clamp。只选**互不重叠**的 bullet。
12. **C 公共头不含非白名单依赖**。任何往 `include/media_engine/*.h` 加 `#include` 的改动必须在 commit body 里显式论证。
