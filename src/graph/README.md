# src/graph/

**职责**：定义 Node 数据结构（多 I/O Port）、Graph 数据结构（带命名 terminal）、Builder API、compile Timeline 段 → Graph。**没有 execution 逻辑**——所有运行时状态都在 scheduler。

See `docs/ARCHITECTURE_GRAPH.md` §用户面 API + §Graph 内部 for the contract.

## 当前文件

- `types.hpp` — `NodeId` / `PortRef` / `Properties` / `PortId` / `TypeId` 等纯数据类型。
- `node.hpp` — `Node` struct：kind + props + 多 I/O port + affinity hint；纯数据。
- `graph.hpp` / `graph.cpp` — `Graph` 不可变快照 + `Graph::Builder` 链式 API + 命名 terminal（`name_terminal("foo", PortRef{...})`）+ content-hash 稳定摘要（用于 SegmentCache）。
- `eval_context.hpp` — `EvalContext` 用户面参数（time / cancel token / etc.），传到 scheduler::evaluate_port。
- `future.hpp` — `Future<T>`：scheduler 返回的异步句柄，`.await()` 阻塞取结果。

## 边界

- ❌ 不含 kernel 函数（那是 `src/task/`）
- ❌ 不含运行时状态（state / promise / cond_var 都在 `src/scheduler/`）
- ❌ 不拥有 FrameHandle / CodecContext（那是 `src/resource/`）
- ❌ 不知道 Timeline JSON 语法（那是 `src/timeline/`；compile 只消费已 parse 的 `timeline::Timeline`）

## 消费者

- `src/scheduler/` — `evaluate_port<T>(Graph, PortRef, EvalContext)` 派发 Task 并返回 Future。
- `src/orchestrator/exporter.cpp` — 每条 clip 构造 `build_demux_graph(uri)` 返回 `(Graph, PortRef)`，交给 scheduler 求值取 `shared_ptr<io::DemuxContext>`。
- `src/orchestrator/segment_cache.hpp` — 按 Graph::content_hash() 缓存 Graph 实例，timeline 重加载时重用。
