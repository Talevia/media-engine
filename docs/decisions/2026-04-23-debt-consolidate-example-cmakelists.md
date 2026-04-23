## 2026-04-23 — debt-consolidate-example-cmakelists：`me_add_example()` 收口（Milestone §M1-debt · Rubric §5.2）

**Context.** `examples/01_passthrough/` … `06_thumbnail/` 每个子目录之前都各自声明 `add_executable` + `target_link_libraries`（加 01 还有 POST_BUILD 拷 `sample.timeline.json`，加 02/03 还有 `target_include_directories ${CMAKE_SOURCE_DIR}/src`）。6 个子 CMakeLists 合计 14 行，每加一个新 example 约 +3 行样板，且"哪种 example 该走哪条分支"需要每次重新翻例子回忆——典型的 patch 堆而非抽象。新 orchestrator（M1 的 `reencode-multi-clip`、M2 的 multi-track compose、M3 的 effect chain demo…）上线时都会再加一个 `0x_*` 示例，boilerplate 会再复制 N 次。

**Decision.** `examples/CMakeLists.txt` 顶部新增 `me_add_example(<name>)` 函数，用 `cmake_parse_arguments` 吃 3 个维度的 opt-in：

- `LANG cpp` — 默认 `main.c`，`LANG cpp` 切 `main.cpp`（只有内部验证工具 02/03 用）。
- `INTERNAL` — 给 target 加 `PRIVATE ${CMAKE_SOURCE_DIR}/src` 的 include，表示这个 example 故意去碰 engine 内部头（02_graph_smoke / 03_timeline_segments）。函数注释里写死"Public demos must not need this flag"，把约束写在抽象里而不是靠 code review。
- `EXTRA_LIBS <targets…>` — PRIVATE 追加的 link targets（02 要 Taskflow）。
- `COPY_RESOURCE <filename>` — 等价于 01 的 POST_BUILD 拷 `sample.timeline.json` 动作。

每个子目录 CMakeLists 现在都是一行：
```
me_add_example(01_passthrough COPY_RESOURCE sample.timeline.json)
me_add_example(02_graph_smoke LANG cpp INTERNAL EXTRA_LIBS Taskflow)
me_add_example(03_timeline_segments LANG cpp INTERNAL)
me_add_example(04_probe)
me_add_example(05_reencode)
me_add_example(06_thumbnail)
```

链接目标统一用 alias `media_engine::media_engine`——之前 02 / 03 写的是直接名 `media_engine`，在当前仓库里等价（alias 指向同一个 static lib），改成 alias 是与公开 demo 写法一致、未来 `install(EXPORT)` 切 SHARED 也不需要再调。

**Alternatives considered.**

1. **自动探测 `main.c` 和 `main.cpp`** 代替 `LANG cpp`——拒：magic 行为在 example 阶段就增加 cognitive load，且允许两个文件同时存在时的歧义。显式 keyword 更可靠。
2. **把 `INTERNAL` 换成全局开关**（比如 `set(EXAMPLE_INTERNAL TRUE)` 给 02/03 目录）——拒：`cmake_parse_arguments` 的 keyword 是 CMake 社区推荐的 "named args" 模式（Modern CMake Cookbook / Effective Modern CMake），在 examples/CMakeLists.txt 顶部集中定义一次比每个子目录自己 set 变量可读。
3. **只收口 4 个纯 C example**（01/04/05/06），02/03 保持原样——拒：留着 2 个不走 helper 的 special case 等于抽象没封口，下一轮"再加个 example"时又会有人对着 02 抄。
4. **把 `media_engine::media_engine` alias 和 `media_engine` 并行支持**——拒：现在是静态链接 alias 无副作用；未来 SHARED 切换需要 alias。统一用 alias 一次性定性。

业界共识来源：CMake 官方 `cmake_parse_arguments` 文档 + Kitware 自己的 `testing/CMake/TestDriver.cmake` 都用这个 keyword-args 模式给 "helper that stamps out N similar targets" 的场景；Abseil、Taskflow 自己的 tests 目录也都走 `absl_cc_library` / `tf_add_test` 这类 helper 而不是复制每个 target。这条路径在 cmake 生态里是 idiomatic。

**Coverage.**

- `rm -rf build && cmake -B build -S . -DME_BUILD_TESTS=ON -DME_WERROR=ON && cmake --build build` clean，6 个 example + 7 个 test 全过。
- `ctest --test-dir build` 7/7 绿。
- `01_passthrough examples/01_passthrough/sample.timeline.json /tmp/me-pt.mp4` 端到端 2s MP4 产出（ffprobe duration=2.000000），确认 `COPY_RESOURCE` post-build 语义未变。
- `ls build/examples/01_passthrough/sample.timeline.json` 确认 POST_BUILD 拷贝动作仍发生。

**License impact.** 无新依赖。纯 CMake 重组。

**Registration.** 无 C API / schema / kernel 注册点变更。CMake 侧动了：
- `examples/CMakeLists.txt` 新增 `function(me_add_example)`（作用域 examples/ 子树，不泄露到 src/ 或 tests/）。
- `examples/0[1-6]_*/CMakeLists.txt` 全部降为单行 `me_add_example(...)` 调用。
- `02_graph_smoke` / `03_timeline_segments` 的 link target 从直接名 `media_engine` 切到 alias `media_engine::media_engine`（语义等价）。
