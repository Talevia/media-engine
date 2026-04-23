## 2026-04-22 — engine-owns-resources（Milestone §M1 · Rubric §5.2）

Commit: `<this>`

**Context.** graph-task-bootstrap 留了 `resource::FramePool` / `resource::CodecPool` / `sched::Scheduler` 三个长生命周期对象没人管——02_graph_smoke 里用 stack 局部对象 workaround。接下来的 orchestrator-bootstrap 要在 engine 外部随用随取，所以先让 `me_engine` 持有它们，消除"谁创建谁销毁"的模糊地带。

**Decision.** `src/core/engine_impl.hpp` 加三个 `unique_ptr` 字段（frames / codecs / scheduler），声明顺序 = 构造顺序；`me_engine_create` 按顺序 `make_unique` 它们，`me_engine_destroy` 让 unique_ptr 反序析构，scheduler 先走确保 tf::Executor 等所有任务结束再释放 pool。配置来源——`cpu_threads = config.num_worker_threads`、`FramePool` 预算 = `config.memory_cache_bytes`。任一初始化抛异常则 catch-all set_error + delete e + 返 `ME_E_INTERNAL`。

**Alternatives considered.**

1. **让 engine 持有引用/指针由外部构造**——engine ABI 暴露构造器让调用方传 pool。被拒：破坏 C API 稳定性；增加调用方心智负担；违反 VISION §7 "No global state" 的企业是"都在 engine handle 下"原则。
2. **全局单例 FramePool / Scheduler**——简单但违反 VISION §7 "multi-engine hosts without surprise"；不同 engine 的 budget / cpu_threads 要能独立配置。
3. **懒初始化（第一次 render 时再创建）**——看似省启动成本，但把错误时机从 create 推到 render，调用方难以及早发现资源不可用。eager 更符合 API.md 的"create 返回即可用"。
4. **把 CodecPool 也先 stub**（只 frames + scheduler）——被拒：Scheduler 构造签名需要两个 pool 引用，留缺口只会让签名反复改；空类开销为零。

**Coverage.** 清洁重 build 后三个 example 全过（01_passthrough / 02_graph_smoke / 03_timeline_segments）——证明 engine 生命周期正常、Scheduler dtor 的 wait_for_all 不 hang、FramePool/CodecPool 默认状态对 scheduler 无害。

**License impact.** 无新依赖。
