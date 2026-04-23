# src/task/

**职责**：Kernel 注册机制、TaskKindId 枚举、多 I/O port/param schema、`TaskContext`。**不含 Task 运行时对象**（那是 scheduler 内部类型）。

See `docs/ARCHITECTURE_GRAPH.md` §Task 运行时与 Kernel 注册 for the contract.

## 当前状态

**Scaffolded, impl incoming**——本目录只有这个 README。具体文件由 backlog `graph-task-bootstrap` 实装。

## 计划的文件

- `task_kind.hpp` — `enum class TaskKindId`，`KindInfo`，`PortDecl` / `ParamDecl` schema 描述
- `context.hpp` — `TaskContext`、`InputValue` / `OutputSlot` variant
- `registry.hpp` / `registry.cpp` — `register_kind(KindInfo)` / `register_variant(...)` / `lookup(TaskKindId)` / `best_kernel_for(kind, affinity_hint)`

## 首批 kind（由各自 backlog 实装）

- `io::demux` / `io::decode_video` / `io::decode_audio`（M1）
- `algo::content_hash` / `algo::thumbnail`（M1）
- `render::compose_cpu`（M2）
- `render::cross_dissolve`（M2）
- `render::effect_chain_gpu`（M3）
- `audio::mix` / `audio::resample` / `audio::timestretch`（M4）

## 边界

- ❌ 不知道 Graph 结构（kernel 只看到 `InputValue[]` 和 `OutputSlot[]`）
- ❌ 不知道 scheduler / thread pool（affinity 是 hint，不是自己派发）
- ❌ 不 capture 资源（`FramePool` 等全部通过 `TaskContext` 在 dispatch 时注入）
