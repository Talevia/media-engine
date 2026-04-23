# src/graph/

**职责**：定义 Node 数据结构（多 I/O Port）、Graph 数据结构（带命名 terminal）、Builder API、compile Timeline 段 → Graph。**没有 execution 逻辑**——所有运行时状态都在 scheduler。

See `docs/ARCHITECTURE_GRAPH.md` §用户面 API + §Graph 内部 for the contract.

## 当前状态

**Scaffolded, impl incoming**——本目录只有这个 README。具体文件由 backlog `graph-task-bootstrap` 实装。

## 计划的文件

- `types.hpp` — `NodeId / NodeRef / PortRef / InputPort / OutputPort / Properties / TypeId` 等纯数据类型
- `node.hpp` — `Node` struct 定义（纯数据）
- `graph.hpp` / `graph.cpp` — `Graph` + `Graph::Builder` + 命名 terminal
- `fragments.hpp` — builder helper 复用函数的声明（具体 helper 由 render / io 模块提供）
- `compiler.hpp` / `compiler.cpp` — `compile_segment(timeline, seg) → Graph`

## 边界

- ❌ 不含 kernel 函数（那是 `src/task/`）
- ❌ 不含运行时状态（state / promise / cond_var 都在 `src/scheduler/`）
- ❌ 不拥有 FrameHandle / CodecContext（那是 `src/resource/`）
- ❌ 不知道 Timeline JSON 语法（那是 `src/timeline/`；compile 只消费已 parse 的 `timeline::Timeline`）
