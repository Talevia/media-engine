## 2026-04-22 — 五模块执行架构（graph / task / scheduler / resource / orchestrator）

本 commit 只落文档 + 目录骨架，代码由后续 backlog 陆续实装。

**Context.** 当前 M1 骨架里 `me_render_start` 直连 `remux_passthrough` 单线程跑 passthrough——撑不到 M2+（多轨合成、GPU effect、frame server、增量缓存、同一 Timeline 按时间分段对应不同 Graph）。用户希望把架构一次性钉死，后续 milestone 只在框架里填肉。设计过程在对话里经历多轮收敛（起初是 Graph as Node / Composite → 发现硬伤 → 最终落地"Node/Graph 纯数据 + Kernel 独立注册 + Task 运行时短命"三分离），完整记录见 plan 文件 `~/.claude/plans/propose-velvety-token.md`。

**Decision.** 五模块职责矩阵（详细见 `docs/ARCHITECTURE_GRAPH.md`）：

- **`graph/`** — Node / Graph / Builder，纯数据，多输入多输出 Port、命名 terminal；无 execute、无函数指针、无 vtable；可序列化、可哈希、可 diff。
- **`task/`** — TaskKindId 枚举 + `KernelFn` 注册表 + `TaskContext` + port/param schema。Kernel 是无捕获纯函数指针，**永不**挂在 Node 上。
- **`scheduler/`** — 对 `(Graph, terminal PortRef, EvalContext)` 产出 `Future<T>`；内部造 `EvalInstance`、派 Task（短命运行时对象）、按 Node.affinity 入 CPU / GPU / HwEnc / IO 异构池；在 dispatch 时把 Resource 注入 TaskContext。
- **`resource/`** — FramePool / CodecPool / GpuContext / Budget；refcounted，不关心 Node / Graph / Task。
- **`orchestrator/`** — Previewer / Exporter / Thumbnailer；**持 Timeline**（非 Graph），按 `timeline::segment()` 切段，缓存 per-segment Graph；自带 encoder / muxer / throttle 状态；不拥有 Node、不定义 kernel、不是 editor。

关键特性：
- Graph 只描述单帧；时间是 `EvalContext.time`，不是 Graph 状态
- 同一 Timeline 对应多个 Graph（段内恒定、段间可换），Orchestrator 按需 compile + 缓存
- Node 支持多 input / 多 output（demux 有 video/audio/metadata 三 output；compose 有 N 个 layer input）
- Graph 有命名 terminal（"video" / "audio"），scheduler `evaluate_port` 按 port 驱动
- Future 是 lazy、scheduler-driven；`await()` 才触发派发
- 确定性：Node content_hash 递归稳定；同 Graph 不同 EvalInstance 并发互不干扰；HW encoder 路径显式标非 bit-identical
- 新依赖：**Taskflow (MIT)** —— CPU pool 的 work-stealing executor

本轮产出（单 commit 覆盖）：
1. `docs/ARCHITECTURE_GRAPH.md` 新建（权威文档）
2. `docs/ARCHITECTURE.md` 更新（Module layout 加五新目录、依赖表加 Taskflow、Current implementation state 按五模块重排）
3. `src/graph/README.md`、`src/task/README.md`、`src/scheduler/README.md`、`src/resource/README.md`、`src/orchestrator/README.md` 五个空壳 README
4. `docs/MILESTONES.md` 加 M1 exit criterion：五模块骨架就位
5. `docs/BACKLOG.md` 加 7 条新 bullet（3 P0 / 4 P1）
6. `CLAUDE.md` Read order 加 `ARCHITECTURE_GRAPH.md`，Architecture invariants 加第 9 条（五模块职责不混）
7. 本归档文件

**Alternatives considered.**

1. **Graph-is-a-Node Composite**（第一次迭代）—— Graph 继承 Node，嵌套 Graph 语义天然、子图可复用。**被拒**：运行时嵌套导致 `content_hash` 递归成本高、`Future` 类型擦除复杂、Chrome trace dump 不干净。改用"builder helper 函数组合（返 NodeRef）"达到同等复用效果，compile 后 Graph 天然扁平。
2. **Futures 独立链起来即图**（HPX / eager std::async 风格）—— 没有显式 Graph 数据结构，所有 DAG 关系隐在 Future 引用里。**被拒**：content hash 不稳定（走链路递归、指针顺序敏感）；跨调用缓存键对不上；拓扑 dump / trace JSON 要爬引用图重建；`std::future` 的 eager 语义会把 1000 帧一次性 enqueue 爆内存预算。
3. **Intel TBB `tbb::flow::graph`** 替代 Taskflow —— Apache-2.0 license 同样干净。**被拒**：flow graph 在 TBB 里是二等公民（文档原话 "used less frequently"），主力是 `parallel_for`；对异构（GPU / HW encoder）的支持比 Taskflow 弱；体积更重。
4. **marl fiber scheduler** 作核心 —— Apache-2.0，Google 用在 SwiftShader / Dawn。**被拒**：纯 fiber 池没原生 DAG；所需的依赖追踪得手写 `WaitGroup`；frame buffer 生命周期没好抽象。留作未来 I/O fiber 可选项，不 day-1 引入。
5. **Graph-as-Orchestrator-input**（即 orchestrator 收 Graph 而非 Timeline）—— 用户一轮反馈澄清：同一 Timeline 按时间分段对应**不同**的 Graph（转场过渡期 active clip 集变化），orchestrator 必须持 Timeline 才能按段切分。**被拒**采用前一版。
6. **Tile-based 执行**（Blender 合成器旧模式）—— 4K+ 场景内存局部性好。**被拒**：phase-1 架构定型范围外；确定性硬约束下 tile 调度顺序风险高；全帧 + 优化的内存池在 1080p 场景够用；真撞瓶颈再加 tile 层。

**Coverage.** 本轮 commit 本身不包含代码改动，无单测覆盖。后续 backlog 的验证规划：
- `graph-task-bootstrap`：单 Node（双 output demux stub）的 Graph 能被 `scheduler.evaluate_port` 求值
- `timeline-segmentation`：单 clip → 1 段、双 clip + cross-dissolve → 3 段两种 case 单测
- `refactor-passthrough-into-graph-exporter`：`01_passthrough` 端到端仍绿，passthrough 行为 bit-identical
- `taskflow-integration`：3-节点 DAG 观察到 2+ 核心并发占用
- 多 output 缓存：同 Node 两个 output port 独立缓存
- 确定性回归：同 timeline 两次 export 字节 diff
- 缓存回归：同 timeline 两次 preview 第二次命中缓存

**License impact.** 新依赖 Taskflow（MIT）。在 `docs/ARCHITECTURE.md` 依赖白名单已添加条目。其他依赖（FFmpeg LGPL build、nlohmann::json MIT）不变。
