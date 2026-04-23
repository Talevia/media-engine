## 2026-04-22 — graph-task-bootstrap（Milestone §M1 · Rubric §5.1 + §5.2）

**Context.** 上一个 commit（`taskflow-integration`）把 Taskflow 的 FetchContent 和链接管道打通；`src/graph/` / `src/task/` / `src/scheduler/` / `src/resource/` 还只有 README，五模块约定的基础类型都没落地。这次把它们实装到"单 Node 的 Graph 能被 scheduler.evaluate_port 求值跑出 kernel 并产出 output"，让后续所有 backlog 可以在类型层对齐到 `docs/ARCHITECTURE_GRAPH.md`。

**Decision.** 按 ARCHITECTURE_GRAPH.md 落地核心类型 + 最小执行环路：

- **`src/graph/types.hpp`**：`NodeId` / `PortRef` / `TypeId` / `InputValue` / `OutputSlot` / `InputPort` / `OutputPort` / `Properties`。`InputValue` 是 `std::variant<monostate, int64_t, double, bool, string, shared_ptr<FrameHandle>>`——bootstrap 够用，audio/metadata/packet stream 未来按枚举 append 加。Properties 用 `std::map<string, InputValue>`（有序，确保 content_hash 跨平台/跨进程一致）。
- **`src/graph/node.hpp`**：`Node` struct 纯数据（kind、props、inputs、outputs、content_hash、time_invariant），无方法。
- **`src/graph/graph.hpp/cpp`**：`Graph` + `Graph::Builder`。Builder.add 查 `task::registry` 的 schema 自动填 input/output Port 类型信息；build() 做 Kahn 拓扑 + 自底向上 FNV-1a content_hash；freeze 后完全 immutable。`std::initializer_list<PortRef>` 重载让调用方可以 `b.add(kind, props, {pa, pb})` 而不必手构 span。
- **`src/graph/future.hpp`**：`Future<T>` 模板。`run_future_` 是 `std::shared_future<void>`（保活 tf::Taskflow + tf::Future）；`eval_` 保活 EvalInstance；`terminal_` 指向要 extract 的 output port。`await()` 等 run_future → 检查 error → `std::get<T>` variant。**不是 std::future 的 eager 语义**，提交时机由 scheduler.evaluate_port 控制。
- **`src/graph/eval_context.hpp`**：`EvalContext` struct（time + resource pointers + cancel + cache）。
- **`src/task/task_kind.hpp`**：`enum class TaskKindId`（dense 值 + 分段：0x0001-bootstrap / 0x1xxx-io / 0x2xxx-algo / 0x3xxx-render / 0x4xxx-audio）、`Affinity` / `Latency`、`KernelFn` typedef、`ParamDecl` / `KindInfo`。
- **`src/task/context.hpp`**：`TaskContext` — dispatch 时 scheduler 填，kernel 读取。
- **`src/task/registry.hpp/cpp`**：全局 registry（mutex-guarded std::map），`register_kind` / `register_variant` / `lookup` / `best_kernel_for(kind, affinity_hint)`。`reset_registry_for_testing` 给测试用。
- **`src/resource/frame_pool.hpp/cpp`**：`FrameSpec` / `FrameHandle`（bootstrap 是 `vector<byte>` 包装）/ `FramePool`（每次 fresh alloc，pressure 恒 0）。`CodecPool` / `GpuContext` 空类。real pooling / LRU / budget 由 `engine-owns-resources` 接上。
- **`src/scheduler/eval_instance.hpp/cpp`**：`EvalInstance` 持 per-node `outputs_` / `inputs_` / `states_` / `cancel_flag` / `error` slots。多 EvalInstance 可对同一 Graph 并发求值互不干扰。
- **`src/scheduler/scheduler.hpp/cpp`**：`Scheduler` 类，`tf::Executor cpu_`；`evaluate_port<T>` 返 `Future<T>`。内部 `build_and_run` 对 Graph 的每个 Node 在 tf::Taskflow 里 emplace 一个 task，按 inputs 连边 `source.precede(this)`，`executor.run(flow)` 立即提交；返回 (`shared_future<void>`, `shared_ptr<EvalInstance>`) pair。

Smoke 验证（`examples/02_graph_smoke/main.cpp`）5 个 case 全过：
1. 单 Node graph：TestConstInt(42) → 42
2. Diamond：const(3) + const(4) via TestAddInt → 7
3. 确定性 hash：同输入建两次 graph，content_hash 相同（0x8cf29f226f62b40）
4. 命名 terminal 查找 + miss 返回 nullopt
5. Kernel 返 ME_E_INTERNAL → `Future::await` 抛 `std::runtime_error`

这个 example 刻意走内部 header（`src/` 下的 .hpp）而不是公共 C API——graph / task / scheduler 是实现细节，对用户只通过 orchestrator 暴露。该 example 是内部验证工具，非 API demo。

**Alternatives considered.**

1. **Node 基类 + virtual execute()**（第一版架构设想的 Composite）—— 已在架构决策里拒掉，这次实装坚持"Node 纯数据"。好处：零 vtable 成本、完全可序列化、content_hash 稳定、跨 arena 缓存 trivially 共享。代价：kernel 得从 registry 查，多一次间接；对这个场景可忽略。
2. **Properties 用无序 `unordered_map`**—— 被拒：迭代顺序非确定，会导致 content_hash 在不同 libstdc++ 版本下输出不同字节，直接破坏 VISION §3.1 确定性。`std::map` 有序，O(log N) 足够（properties 数量小）。
3. **直接用 `tf::cudaFlow` / 让 Taskflow 管 GPU**—— Taskflow 的 GPU 支持是 CUDA-only，和 bgfx / Metal / Vulkan 不对齐；且 VISION 红线禁 CUDA 专属链（NVIDIA 绑定）。GPU 调度在 M3 通过 `scheduler::GpuQueue` 自研，Taskflow 只管 CPU 池。
4. **FNV-1a hash 用 xxhash**—— xxhash 更快、碰撞更低，但要新增依赖或引入 header。FNV-1a 足够 bootstrap（properties + kind 的 hash 量级很小），性能差异不影响任何现实场景。接口没变，将来可无缝替换。
5. **Registry 放 `static thread_local`**—— 让多测试 fixture 隔离。被拒：kernel 是全局能力，process-wide 单例更符合真实使用；测试隔离用 `reset_registry_for_testing()` 显式清表更直观。
6. **Future 用 `std::promise` / `std::future` 直接包装**—— 被拒：会引入多一层 set_value 调用（scheduler.dispatch 完后还得 promise.set_value），tf::Future 已经是 shared_future-like，多一层无意义。
7. **`Graph::add` 接 `vector<PortRef>` 而非 span**—— 被拒：会强制每次调用堆分配。加一个 `initializer_list` 重载满足 brace 语法同时保留 span 路径零分配。

**Coverage.** `examples/02_graph_smoke` 5 个 case 全过；`examples/01_passthrough` 回归仍绿（C API 行为未动、stub 数未变）。`clang++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter` 在所有新 `.cpp` / `.hpp` 上通过。

**License impact.** 无新依赖。已有 Taskflow（MIT，上一 commit）。
