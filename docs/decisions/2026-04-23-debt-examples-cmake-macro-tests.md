## 2026-04-23 — debt-examples-cmake-macro-tests：me_add_example 参数校验收紧 + post-subdir self-check（Milestone §M2-prep · Rubric §5.2）

**Context.** `examples/CMakeLists.txt:16-40` 的 `me_add_example()` 函数（由 `debt-consolidate-example-cmakelists` cycle 引入）把 6 个 example 子目录从 14 行样板压到 1 行调用，但四个 option 组合（`LANG cpp|<empty>` / `INTERNAL` / `EXTRA_LIBS` / `COPY_RESOURCE`）**没有参数验证**：

- `me_add_example(foo INERNAL)`（typo，少 T）：`cmake_parse_arguments` 把 `INERNAL` 扔进 `ARG_UNPARSED_ARGUMENTS`，函数往下跑，`ARG_INTERNAL` 为空 → 静默产出 **不带 src/-include** 的 target。如果 foo 是 `02_graph_smoke` 类 internal example，它构建时抱 "private header not found"，错误信息完全不指向 typo。
- `me_add_example(foo LANG c)`：`LANG` 只分派 `"cpp"`；任何其他值 silently 走 C 分支。`LANG c` 语义上等价于默认，但 `LANG cpp_with_typo` 同样 silently 走 C → 编译会挂在 `.c`-as-C++ 语法错。
- `me_add_example(foo COPY_RESOURCE sample.json)` 当 sample.json 不存在：POST_BUILD `copy_if_different` 在 build 时才报"source file does not exist"，configure 时安静。

`grep -rn 'me_add_example' tests/ returns empty` —— 功能靠 6 个 example 间接验证；typo 不 fail CI，只 fail build，报错信息不精确。

**Decision.** 不走"CMake unit test"路径（需要 ctest subcommand-launch + isolated CMake 子进程，复杂度高不值），走 bullet direction 建议的"函数内强校验 + configure 期自检 block"折衷：

1. **`me_add_example()` 函数内新增 4 条 validation，任一失败 `message(FATAL_ERROR ...)`**：
   - `name` 非空（防 `me_add_example()` 没给首参）。
   - `ARG_UNPARSED_ARGUMENTS` 为空——任何 typo / unknown option 都进这个 list，这是最高 ROI 的 typo guard。err message 列出 offending options + 枚举 valid options 帮 author 纠正。
   - `ARG_LANG` 必须为空或 `"cpp"`——明示什么是 legal。
   - `ARG_COPY_RESOURCE`（if 设）指向的文件必须存在——configure 时 catch，不用等 POST_BUILD。
   - 新增：`${_src}`（解析后的 main.c / main.cpp）在 CMAKE_CURRENT_SOURCE_DIR 下必须存在——防 `LANG cpp` 用了但目录只有 main.c 的情况。

2. **`examples/CMakeLists.txt` 顶部加 `_me_expected_examples` 列表**（跟 `add_subdirectory` 调用 lock-step 同步），`add_subdirectory` 全部完成后跑两段 foreach self-check：
   - **Existence**：`if(NOT TARGET ${_expected})` → FATAL_ERROR，消息提示清 build 目录重配。catch "subdir 没调 me_add_example 或 add_subdirectory call 漏了"。
   - **Link invariant**：每个 target 的 LINK_LIBRARIES 必含 `media_engine::media_engine`（`IN_LIST` check）。catch "subdir 绕过 me_add_example 自己 add_executable 忘了 link" 的 case。

3. **负路径手工验证**（not CI-enforced）：本 cycle 用 `cmake -P /tmp/negcheck.cmake` 外加调用 `me_add_example(... GARBAGE_OPTION)`，确认 FATAL_ERROR 的 stderr 输出格式正确（"me_add_example(dummy_target): unknown option(s): GARBAGE_OPTION"）。把结果写进本 decision；不加进 ctest suite——原因下文 alternatives。

**Non-goals.**

- **不**加 CMake-level unit test suite（`add_test(COMMAND cmake -P ... WILL_FAIL true)`）。三个原因：
  a. 需要从 examples/CMakeLists.txt 把 `me_add_example` 函数抽到独立 `cmake/me_add_example.cmake` 模块才能 cmake -P include——是 layering refactor 而非 debt 整理。
  b. CMake `WILL_FAIL` + exact `FAIL_REGULAR_EXPRESSION` 脆弱，CMake 版本升级的 error message 微调会触发 false failure。
  c. 现有 6 个 example 的 configure success 已是 **positive integration test**；post-subdir self-check 把"invariants hold"钉成 configure-time assertion，覆盖度足够。
- **不**给 `INTERNAL` / `EXTRA_LIBS` 加 allowlist 校验（e.g. "INTERNAL 必须是某些特定 example"）——它们是设计意图内的 free-form option。

**Alternatives considered.**

1. **把 `me_add_example` 抽出独立 cmake module** + ctest-wrapped negative cases — 拒：layering refactor 超出 debt scope；本 cycle 只收紧，不动 layout。未来 examples 超过 10 个或需要复用到非-examples 目录时再做。
2. **在 `ARG_UNPARSED_ARGUMENTS` 非空时只 `message(WARNING ...)` 不 FATAL** — 拒：warning 在 cache 重配时容易被忽略；FATAL 是 typo guard 的正确强度。
3. **给 `ARG_LANG` 增加 `"c"` 作 explicit 同义词** — 拒：添加多一种有效值 = 更多歧义；保持"不写或 `cpp`"二选一更清晰。未来想支持 Objective-C/swift 再 append。
4. **把 `_me_expected_examples` 改成 glob `file(GLOB ...)` 自动发现子目录** — 拒：显式列表 ≫ glob，CMake docs 明确推荐不用 GLOB 收集 source；同样逻辑适用于 "有哪些 example"。同步成本低（加新 example 改两处已是 lock-step 显式）。
5. **检查 `EXTRA_LIBS` 里的每个 target 都存在（`if(TARGET ${lib})`）** — 拒：EXTRA_LIBS 可能是 FFmpeg 的 imported target 或 subdir 里 include 之前的 target，提前 check 会误触发。target_link_libraries 本身在 build 时报 undef 也够用。
6. **用 `cmake_parse_arguments(... PARSE_ARGV 1 ...)` 新语法** — 拒：和现有 `${ARGN}` 调用等价，机械换。

业界共识来源：CMake Cookbook 关于 `cmake_parse_arguments` 的典型用法（`ARG_UNPARSED_ARGUMENTS` 校验是"每个 wrapper function 必做"的章节建议）、Catch2 / spdlog 等 CMake 重度项目的 helper function 也都带 typo guard。

**Coverage.**

- `cmake --build /Volumes/Code/media-engine/build`（OCIO=OFF 老 build dir）重配 + 重建 clean。
- `ctest` 16/16 suite 绿；post-subdir self-check 两段 foreach 没触发 FATAL_ERROR（证实 6 个 example target 都正常创建、都链接 media_engine）。
- 负路径手工 `cmake -P neg.cmake` 确认 FATAL_ERROR stderr 格式正确（本 decision 文内引用）。
- 无代码 / API / schema 改动；此 cycle 纯 CMake hygiene。

**License impact.** 无。

**Registration.**
- `examples/CMakeLists.txt`：`me_add_example` 里 5 段 validation（unparsed args、name 空、LANG 值、COPY_RESOURCE 存在、_src 存在）；顶部加 `_me_expected_examples` 列表；底部加两段 foreach self-check（target existence + link invariant）。
- 无其他文件改动。

**§M 自动化影响.** 本 cycle 是 debt 类（M2-prep tag），不对应任何 M2 exit criterion。§M.1 evidence check 不会动任何打勾状态。§M.2 跳过。
