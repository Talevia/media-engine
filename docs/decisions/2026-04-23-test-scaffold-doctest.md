## 2026-04-23 — test-scaffold-doctest (Milestone §M1 · Rubric §5.2)

**Context.** M1 exit criterion「单元测试框架接入（doctest），至少 1 条通过的 passthrough 确定性回归」。之前只有 `ME_BUILD_TESTS` CMake option + `enable_testing()` 空壳，没有任何 `tests/` 目录、没有任何 `add_test`。前两个 cycle（probe-impl / reencode-h264-videotoolbox）一直靠 examples + 手工 ffprobe 验证，缺少能 regression 的细粒度断言——C API 的 null-safety、status enum 覆盖、schema rejection 路径等都没有机器化守护。

**Decision.**
- FetchContent doctest v2.4.11（MIT，白名单依赖）。拉的是整个 repo，但用 `CACHE INTERNAL ""` 关掉 `DOCTEST_WITH_TESTS / WITH_MAIN_IN_STATIC_LIB / NO_INSTALL`，只保留 header-only 接口 target `doctest::doctest`。
- `tests/` 目录下 **每个 test suite 一个 executable**：`test_status / test_engine / test_timeline_schema`。共享一个 `test_main.cpp`（只包 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`）。一个 suite 崩了不影响另一个，`ctest` 的失败定位也贴近文件名。
- `add_test(NAME <suite> COMMAND <suite>)` 注册到 ctest；`cmake -DME_BUILD_TESTS=ON -B build && cmake --build build && ctest --test-dir build` 是开发者本机的单命令回归。
- 覆盖的 3 面：
  - `test_status`：所有 `ME_OK / ME_E_*` 全量走一次 `me_status_str`，断言非 null、非空、互相不等；加 out-of-range enum 值不崩；`me_version()` 字段守护。
  - `test_engine`：`me_engine_create(NULL, &e)` OK、`NULL out` 返回 `ME_E_INVALID_ARG`、`me_engine_destroy(NULL)` 无崩、fresh engine 的 `me_engine_last_error` 返 `""`、`last_error(NULL)` 返 sentinel、`me_engine_config_t` 传自定义值 OK。
  - `test_timeline_schema`：有效 timeline loads 且字段读出正确；`schemaVersion=2` → `ME_E_PARSE`；malformed JSON → `ME_E_PARSE`；多 clip → `ME_E_UNSUPPORTED`；`clip.effects` → `ME_E_UNSUPPORTED`；`load_json(NULL engine)` → `ME_E_INVALID_ARG`；rejection 后 `me_engine_last_error` 包含 "schemaVersion" 关键词（让 last_error propagation 也进回归）。
- `CMAKE_POLICY_VERSION_MINIMUM 3.5` 在 `FetchContent_MakeAvailable(doctest)` 之前设——doctest v2.4.11 的 `cmake_minimum_required(VERSION 2.8)` 低于 CMake 4.x 强制地板，不设这行 configure 直接失败。PAIN_POINTS.md 有对应条目，上游升级后删。
- 顶层 `CMakeLists.txt` 的 `if(ME_BUILD_TESTS)` 分支现在做完 `enable_testing()` 后 `add_subdirectory(tests)`，其余构建目标不受影响（默认 `ME_BUILD_TESTS=OFF`）。

**Alternatives considered.**
- **Catch2**：更流行，宏更接近 BDD 风格；但编译时间显著更长（Catch2 v3 amalgamated TU ~30s vs doctest <2s），M1 bootstrap 阶段更偏重 fast feedback loop；doctest 的 compile-time 是其最大卖点。拒。
- **GoogleTest**：社区最大，但依赖 GMock/GTest 两个 target + 需要 pthread/Abseil 等间接依赖，FetchContent 下来约 5k 文件；doctest 单 header。拒。
- **自研 minimal TAP runner**：零依赖最干净，但失去 fixture / filter / `REQUIRE` 这些"免费"能力，第一批 6 条测试就要自己写运行器，跑偏。拒。
- **把 test 文件 append 到 src/**：跟 src 同 target 编，节省一个 target，但让 media_engine library 带上 test 入口符号、污染 install target；M6+ host binding 时这些符号会漏给 host。拒——tests 独立 executable 隔离干净。
- **单 executable 串所有 suite**：启动开销小但一处段错误干掉全部。doctest 支持一个 binary 多 suite，但我们主动拆成每 suite 一 binary 就是为了 regression 精度。拒。
- **把 timeline rejection 测试做成 "timeline builder + mutation" 生成器**：更可扩展，但 5 条 mutation 下还太早；PAIN_POINTS 记录，待 schema v2 / multi-clip 落地后一起抽。

**Coverage.**
- `cmake -B build -S . -DME_BUILD_TESTS=ON && cmake --build build` 全绿；3 个 test executables 都链接并运行：
  - test_status: 2 TC / 13 assertions
  - test_engine: 6 TC / 10 assertions
  - test_timeline_schema: 7 TC / 29 assertions
- `ctest --test-dir build --output-on-failure` → 3/3 passed in ~1.1s。
- `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON -DME_BUILD_TESTS=ON && cmake --build build-rel && ctest --test-dir build-rel` → 3/3 passed，`-Wall -Wextra -Werror -Wpedantic` 全部 clean（测试代码 tight 到 `-Werror` 这关）。
- `./build/examples/01_passthrough/01_passthrough ...` 回归仍产 2s 合法 MP4，doctest 接入未影响主链路。
- 默认构建（`cmake -B build -S .` 不带 flag）不再触发 doctest FetchContent——`ME_BUILD_TESTS=OFF` 是默认，`add_subdirectory(tests)` 被短路。

**License impact.** 新增依赖 **doctest v2.4.11，MIT 许可**——白名单内（cf. ARCHITECTURE.md 依赖表中 "MIT" 已接受栏位）；纯 header-only，不链运行时。无 FFmpeg 侧变动。

**Registration.** 本轮动的注册点：
- `TaskKindId` / kernel registry: 未动。
- `CodecPool` / `FramePool` / resource factory: 未动。
- Orchestrator factory: 未动。
- 新导出的 C API 函数: 未新增。
- CMake target / install export / FetchContent_Declare: **新增 `FetchContent_Declare(doctest)`** 在 `tests/CMakeLists.txt`；3 个新 executable target（`test_status / test_engine / test_timeline_schema`），`add_test` × 3 注册到 `ctest`。不进 `install()`，不导出。
- JSON schema 新字段 / 新 effect kind / 新 codec 名: 未动。
- 新 source 树: `tests/` 顶层目录 + `test_main.cpp` + 3 个 suite 文件。
- 顶层 `CMakeLists.txt` 的 `if(ME_BUILD_TESTS)` 块新增 `add_subdirectory(tests)`。
