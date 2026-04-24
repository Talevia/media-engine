# src/task/

**职责**：Kernel 注册机制、TaskKindId 枚举、多 I/O port/param schema、`TaskContext`。**不含 Task 运行时对象**（那是 scheduler 内部类型）。

See `docs/ARCHITECTURE_GRAPH.md` §Task 运行时与 Kernel 注册 for the contract.

## 当前文件

- `task_kind.hpp` — `enum class TaskKindId`（`IoDemux`、`Test*` bootstrap kinds）、`KindInfo` 描述符、`PortDecl` / `ParamDecl` schema 描述。
- `context.hpp` — `TaskContext`：kernel 调用时的注入容器，携带 FramePool / CodecPool / GpuContext / Cache 引用 + cancel flag + InputValue / OutputSlot。
- `registry.hpp` / `registry.cpp` — 全局 kernel registry：`register_kind(KindInfo)`、`lookup(TaskKindId)`、`best_kernel_for(kind, affinity_hint)`。Scheduler 派发时查这里。

## 已注册 kinds

- `IoDemux` — `src/io/demux_kernel.cpp` 注册；消费 `uri` property，产出 `shared_ptr<io::DemuxContext>`。orchestrator 的 per-clip `build_demux_graph` 用的就是这个。
- Bootstrap test kinds（`TaskKindId::Test*`） — `tests/test_scheduler_smoke` 用途，生产路径无关。

## 计划中的 kinds（由各自 backlog 实装）

随各自 feature 增长；orchestrator 走的主路径（passthrough / reencode / compose）目前不依赖 kernel 注册外的额外 kinds——Previewer / Exporter / ComposeSink 直接调 io / compose / reencode 函数。Graph eval 只深用 `IoDemux` 一个 kernel。

## 边界

- ❌ 不知道 Graph 结构（kernel 只看到 `InputValue[]` 和 `OutputSlot[]`）
- ❌ 不知道 scheduler / thread pool（affinity 是 hint，不是自己派发）
- ❌ 不 capture 资源（`FramePool` 等全部通过 `TaskContext` 在 dispatch 时注入）
