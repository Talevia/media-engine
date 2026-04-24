# src/scheduler/

**职责**：对 `(Graph, terminal PortRef, EvalContext)` 产出 Future；内部构造 EvalInstance、派发 Task、按 Node.affinity 入异构池、注入 TaskContext、查缓存、处理取消。

See `docs/ARCHITECTURE_GRAPH.md` §Task 运行时与 Kernel 注册 + §确定性 for the contract.

## 当前文件

- `scheduler.hpp` / `scheduler.cpp` — `Scheduler` class：`evaluate_port<T>(Graph, PortRef, EvalContext)` → `Future<T>` 入口；构造 Taskflow executor（CPU pool work-stealing）+ 注入 FramePool / CodecPool 到 TaskContext。`Config{.cpu_threads}` 控制池大小（0 = `hardware_concurrency()`）。
- `eval_instance.hpp` / `eval_instance.cpp` — per-evaluate 运行状态容器：kernel lookup 缓存、promise / future 结合点、cancel flag 订阅。一次 `evaluate_port` 用一个 EvalInstance，短生命。

## 依赖

- **Taskflow** (MIT) — CPU pool 的 work-stealing executor。
- `src/graph/` — 读 Node / Graph / PortRef 结构。
- `src/task/` — 查 kernel registry，获取 kernel function + 注入 TaskContext schema。
- `src/resource/` — FramePool / CodecPool 引用透传到 TaskContext（GpuContext 随 M3 bgfx 路径到位后增补）。

## 计划中（未实装）

- `gpu_queue.hpp/.cpp` — bgfx 串行提交队列，bgfx::frame() single-thread 约束落点。当前 `src/gpu/render_thread.hpp` 承担类似角色，未来 merge 到 scheduler 内部。
- `hw_enc_queue.hpp/.cpp` — VideoToolbox / NVENC / AMF bounded queue。当前 reencode 直接拉 h264_videotoolbox，没队列化——多轨多段 reencode 时可能撞 HW 并发限额。profile 证实瓶颈再抽 pool。
- `io_pool.hpp/.cpp` — 独立阻塞 I/O 线程池。当前 Taskflow executor 承担，文件 I/O / 网络 I/O 混在 CPU 池里；未来按 affinity 分离。

## 边界

- ❌ 不定义 kernel 函数
- ❌ 不解析 Timeline / JSON
- ❌ 不拥有 Node（Node 归 Graph）
- ❌ 不做帧编排（那是 orchestrator；scheduler 只负责"把一次 evaluate 跑完"）
