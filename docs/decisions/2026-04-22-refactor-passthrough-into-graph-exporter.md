## 2026-04-22 — refactor-passthrough-into-graph-exporter（Milestone §M1 · Rubric §5.1 + §5.3）

**Context.** orchestrator-bootstrap 把 C API 接到了 Exporter，但 Exporter::export_to 里仍是"启动 worker → 调 io::remux_passthrough 一把梭"——并没有真走新的 graph/task/scheduler 框架。这次把 passthrough 路径彻底搬到框架里：demux 逻辑注册为 `io::demux` kernel，mux 逻辑归 Exporter 自持的 passthrough_mux helper。产出是"第一个真实通过 scheduler.evaluate_port 跑的 kernel"+"Graph 框架的端到端验证"。

**Decision.**

新增：
- **`src/io/demux_context.hpp/cpp`**：`DemuxContext` RAII 包装 AVFormatContext——只有 open/close + metadata 访问，读 packet 留给下游消费者。
- **`src/graph/types.hpp` 扩 InputValue variant** 加 `std::shared_ptr<io::DemuxContext>`；`TypeId` 扩 `DemuxCtx = 6`（append-only，保持 ABI 排序稳定）。`graph/graph.cpp` 的 `hash_input_value` 已有 catch-all 不 hash shared_ptr 身份——对 stateful runtime object 正确；注释更新。
- **`src/io/demux_kernel.hpp/cpp`**：注册 `TaskKindId::IoDemux` 的 kernel。Schema：inputs=none、outputs=`[source: DemuxCtx]`、params=`[uri: string]`。Affinity = Io（阻塞 I/O），time_invariant = true（相同 URI 开相同 stream）。Kernel 逻辑：strip `file://` 前缀 → avformat_open_input → avformat_find_stream_info → emit shared_ptr<DemuxContext>。
- **`src/orchestrator/muxer_state.hpp/cpp`**：`passthrough_mux(demux, opts, &err)` helper。接 DemuxContext + 输出路径/container/progress_cb/cancel_flag，做 stream-copy。实现搬自原 `io::remux_passthrough`，逻辑等价但解耦。
- **`src/api/engine.cpp`** `me_engine_create` 里 std::call_once 注册 built-in kinds（目前只有 demux）——保证 kind registry 在第一次 engine create 后就有 demux 可查。

改造：
- **`src/orchestrator/exporter.cpp`** 现在：
  1. 构造单节点 Graph（one `io::demux` node，属性 = `{uri}`，命名 terminal "demux"）
  2. 插入 `graph_cache_`（尽管 passthrough 只有一段，缓存路径端到端走了一遍）
  3. `scheduler.evaluate_port<shared_ptr<DemuxContext>>(g, terminal, ctx)` → Future
  4. Future.await() 拿到 DemuxContext
  5. 传给 passthrough_mux 扫包写文件
- scheduler 用的是 engine->scheduler（engine-owns-resources commit 就位的）。Future.await 走 tf::Executor 的 CPU pool——demux kernel 以 Io affinity 注册但 scheduler bootstrap 只有 CPU pool，best_kernel_for 退回 primary kernel——可运行，只是占了 CPU worker；Io pool 到位前都这样，后续 `marl-io-pool` 或类似 backlog 再迁。

删除：
- **`src/io/ffmpeg_remux.hpp` + `ffmpeg_remux.cpp`**——逻辑已拆进 demux_kernel + passthrough_mux，不再需要。`src/CMakeLists.txt` 里移除。

**确定性验证**（本轮最重要的信号）——同一输入跑两次 `01_passthrough`，`cmp -s` 结果 byte-identical。这证明：
- scheduler 调度顺序不影响输出（单 Node 场景平凡，但路径是真走 Taskflow executor）
- passthrough_mux 无非确定性来源（相同 AVPacket 序列、相同 write_frame 顺序）

**Alternatives considered.**

1. **Packet 流作为 graph 的一等类型**（`PacketStream` type，每 packet 一个 InputValue）—— 被拒：和 Graph 的 "单帧 evaluate" 模型不兼容（会变成 N 次 evaluate 每 packet 一次，或改造成 streaming edge 另立一套）。passthrough 的自然粒度是"整个文件"而不是"每个 packet 一帧"，让 DemuxContext 做 handle、orchestrator 做 streaming 更贴合 ARCHITECTURE_GRAPH.md §批编码里"decode is a Node, mux is Exporter state"的拆分。
2. **demux kernel 内部直接做完整 mux**（合并 demux+mux 到一个 kernel）—— 被拒：orchestrator vs graph 的分界就是 "stateful streaming 外挂 / 单帧纯函数"，一把梭会让 mux 能力绑死在某个 kernel 上，后续非 passthrough 编码路径（如 h264_videotoolbox）就得重写。
3. **保留 `io::remux_passthrough` 作为 fallback**—— 被拒：死代码是最贵的 tech debt，保留会导致未来改动两边都要改（或遗漏）；删除后整个 passthrough 行为由 graph 路径覆盖，没有回退口。
4. **把 `graph_cache_.insert` 推迟到 Exporter 开始支持多段 Timeline 再加**—— 被拒：现在插入成本是 O(1)、测试面覆盖到了缓存路径；等多段再补会让多段的 PR 变大；早点 exercise 能更快发现 hash 设计问题。
5. **Io pool 单独起**—— 被拒：超出本 task 范围；Io kernel 暂落到 CPU pool 可接受（demux 是阻塞操作但 bootstrap 数据量小）。

**Coverage.**
- 01_passthrough（C API）→ 30 帧 h264 MP4，1.0s duration，container 正确
- **确定性回归**：两次 passthrough 输出 `cmp -s` byte-identical
- 02_graph_smoke 5/5 通过（graph + task registry 未被改动影响）
- 03_timeline_segments 6/6 通过
- `git grep 'ME_E_UNSUPPORTED' src | wc -l` = 5（不升；这次 task 还是这五个 stub）

**License impact.** 无新依赖。删除 `io/ffmpeg_remux.{hpp,cpp}` 不影响 link 图（FFmpeg LGPL 链接从 demux_kernel / muxer_state 继续）。
