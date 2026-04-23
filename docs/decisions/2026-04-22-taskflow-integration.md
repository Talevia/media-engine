## 2026-04-22 — taskflow-integration（Milestone §M1 · Rubric §5.5 + §5.6）

Commit: `<this>`

**Context.** `docs/ARCHITECTURE_GRAPH.md` 里 scheduler 的 CPU pool 定为用 Taskflow 的 work-stealing executor。架构 commit 落地时已把 Taskflow 加进 `ARCHITECTURE.md` 的依赖白名单，但 CMake 里没实际 FetchContent；第一个真正用 Taskflow 的 `graph-task-bootstrap` 之前，需要先把 CMake 管道 + link 图打通，避免届时 CMake 问题和代码问题混在一起 debug。

**Decision.** `src/CMakeLists.txt` 加 `FetchContent_Declare(taskflow GIT_TAG v3.7.0 SHALLOW)`，禁用其 `TF_BUILD_TESTS / EXAMPLES / BENCHMARKS / CUDA / SYCL / HIP / PROFILER`（只要 header，不要它构建自己的示例工程）。`target_link_libraries(media_engine PRIVATE Taskflow::Taskflow)`。新增 `src/scheduler/taskflow_probe.cpp` 作为 placeholder——跑一个 3-节点 diamond DAG 验证 `tf::Executor` 可运行，加载期触发一次，任何 ABI/version 不匹配会在 `me_engine_create` 之前暴露。该文件将被 `graph-task-bootstrap` 产出的 `scheduler.cpp` 替换。

**Alternatives considered.**

1. **合并进 graph-task-bootstrap**——把 Taskflow 的 CMake 改动、第一次 `tf::Executor` 使用、以及全部 graph/task/scheduler 基础类型放一个 commit。被拒：graph-task-bootstrap 本身已经跨多模块、几百行新代码，把 CMake 变动和依赖下载混进去，一旦 CMake 出问题不容易定位；保持单一职责让这个 commit 的验证面最小（下载 + 链接 + 3 节点 DAG 跑通）。
2. **完全不加 probe 文件，只改 CMake**——FetchContent 但不实际用。被拒：CMake 能下载不等于头文件能在我们的 toolchain（macOS clang C++20）下编译；probe 是轻量编译器验证，十几行，等同于 smoke test。`graph-task-bootstrap` 来了直接删除。
3. **用 Intel TBB 的 `tbb::flow::graph`**——Apache-2.0 license 同样干净。被拒：flow graph 在 TBB 里是二等公民（TBB 文档原话 "used less frequently"），主力是 `parallel_for`；对我们异构 + 精细 DAG 调度需求支持弱；已在 `docs/decisions/2026-04-22-architecture-graph.md` 展开过。
4. **marl fiber scheduler**——Apache-2.0，纯 fiber。被拒：没原生 DAG；DAG 依赖手写 WaitGroup；留作将来 I/O fiber 可选项，不 day-1 引入。

**Coverage.** `clang++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter` 在 macOS clang 16 下单独编译 `taskflow_probe.cpp` 通过。完整 cmake build 等 cmake 安装后验证（当前本地 cmake 未装；FetchContent 本身是 CMake 标准机制，pattern 与已工作的 nlohmann_json 同构）。

**License impact.** 新增依赖 Taskflow v3.7.0（MIT）。`docs/ARCHITECTURE.md` 依赖白名单已列入（上一 commit 架构落地时）。`docs/VISION.md` §3.4 红线不触碰：MIT 在白名单内、header-only、无 GPL 传染。
