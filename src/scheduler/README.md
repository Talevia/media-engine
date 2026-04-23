# src/scheduler/

**职责**：对 `(Graph, terminal PortRef, EvalContext)` 产出 Future；内部构造 EvalInstance、派发 Task、按 Node.affinity 入异构池、注入 TaskContext、查缓存、处理取消。

See `docs/ARCHITECTURE_GRAPH.md` §Task 运行时与 Kernel 注册 + §确定性 for the contract.

## 当前状态

**Scaffolded, impl incoming**——本目录只有这个 README。具体文件由 backlog `graph-task-bootstrap` + `taskflow-integration` 实装。

## 计划的文件

- `scheduler.hpp` / `scheduler.cpp` — `Scheduler` class，`evaluate_port<T>` / `wait<T>` / `submit<T>` 入口
- `eval_instance.hpp` / `eval_instance.cpp` — per-invocation 运行状态
- `gpu_queue.hpp` / `gpu_queue.cpp` — bgfx 串行提交队列（stub，M3 填）
- `hw_enc_queue.hpp` / `hw_enc_queue.cpp` — VideoToolbox / NVENC / ... bounded queue（M1 填 VT）
- `io_pool.hpp` / `io_pool.cpp` — 阻塞 I/O 线程池
- `README.md`

## 依赖

- **Taskflow** (MIT) — CPU pool 的 work-stealing executor
- `src/graph/` — 读 Node / Graph 结构
- `src/task/` — 查 kernel
- `src/resource/` — 注入 FramePool / CodecPool / GpuContext 到 TaskContext

## 边界

- ❌ 不定义 kernel 函数
- ❌ 不解析 Timeline / JSON
- ❌ 不拥有 Node（Node 归 Graph）
- ❌ 不做帧编排（那是 orchestrator；scheduler 只负责"把一次 evaluate 跑完"）
