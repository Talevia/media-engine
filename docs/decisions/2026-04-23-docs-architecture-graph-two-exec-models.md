## 2026-04-23 — docs-architecture-graph-two-exec-models：把两种执行模型写进 ARCHITECTURE_GRAPH.md 顶部（Milestone §M2-prep · Rubric §5.2）

**Context.** `docs/ARCHITECTURE_GRAPH.md` 一直以来只描述 "Graph per-frame + scheduler" 一种执行模型——Previewer / Thumbnailer 的 single-frame 路径。但 M1 的 re-encode / passthrough concat（`reencode_pipeline.cpp` / `muxer_state.cpp`）**没走**那条路径：它们是 orchestrator 自持的 streaming 循环，encoder + muxer 状态活过整个 job。文档里**一句话都没提**这条路径，给新贡献者留的印象是"万物皆 Graph"。

实际后果：最近的 `reencode-multi-clip` cycle 差点把 "open encoder" / "flush encoder" 写进 Task kernel 的直觉——正是被文档误导的那种错。M4 / M5 / M6 新 orchestrator 上线时这种误判会继续发生，每次都要靠作者自己意识到"encoder 有跨帧状态不能塞 kernel"。写进文档把判别标准前置是最便宜的 prevention。

**Decision.** `docs/ARCHITECTURE_GRAPH.md` 在开篇 ASCII diagram + "一句话" 段之后、"关键理念" 小节之前，插入新 `## 两种执行模型` 小节，三块：

1. **(a) Graph per-frame + scheduler — stateless per-frame**：preview / thumbnail / frame-cache / future `me_render_frame` 走这条。Kernel 纯函数、无跨帧状态、调度顺序无关、用 `FrameHandle` 跨调用引用。本文件后续各节描述的就是这条。M1 的 `02_graph_smoke` 是 canonical 例子。
2. **(b) Orchestrator streaming — stateful per-job**：export / re-encode / passthrough concat 走这条。Orchestrator 自持 encoder + muxer + FIFO lifecycle，状态跨 frame / clip / segment。**不经过** Graph / Task / scheduler。`01_passthrough` / `05_reencode` 是 canonical 例子。
3. **何时选哪种** 判别表 + 一句话判别（"合法的 encoder / muxer / FIFO state 是否必须在两次调用间持续"）。还标注两条路径**共用** `resource::*` 和 `timeline::Timeline`——分歧只在状态持续和 Task 物化。

插入位置：文件顶部 scrolling-3-screens-of-the-reader 看到 "一句话：..." 之后**立刻**看到这节，不让读者走过 50 行内部细节才意识到"还有另一种路径"。

**Alternatives considered.**

1. **把 streaming 路径完全另开一份 `ARCHITECTURE_STREAMING.md`**——拒：跨文件 cross-ref 成本比单文件一节高；两种模型共用 resource / Timeline 层，分两份反而让读者觉得它们不相关。
2. **只在 "关键理念" 里加第 9 条**——拒：关键理念是本文件已经立住的 per-frame 约束。streaming 路径明显**违反**其中几条（跨帧状态、non-pure kernel）——不能以"第 9 条"形式静默挂进去。需要独立章节说明"那几条约束只适用于路径 (a)"。
3. **在 orchestrator 目录 README 里写**（`src/orchestrator/README.md`）——拒：orchestrator 目录 README 是代码 navigation 用，不是架构文档。读者从 VISION → ARCHITECTURE → ARCHITECTURE_GRAPH 的顺序进来，第三个文件应该给完整图景。
4. **改写现有 ASCII diagram 把两条路径都画上**——考虑过，暂缓：ASCII diagram 已经密，加一条路径会让它更难读。新章节里用 markdown 表格判别更高效。未来真要画统一 diagram，再独立 cycle 做。

业界共识来源：OBS 的 "Plugin API" 文档区分 "filter frame" vs "encoder session"；NVIDIA CUDA 的 stream-vs-per-frame 模型；DAW 生态里的 "sample-accurate streaming bus" vs "offline render"。媒体引擎里区分 per-frame (stateless) 和 streaming (stateful) 是几十年的共识，我们只是把本仓库的 instance 写下来。

**Coverage.**

- 纯 Markdown 改动，文件末尾新内容 ~50 行，不 reorder 既有章节。
- `cat docs/ARCHITECTURE_GRAPH.md` 通读一遍：新章节 + 既有内容无冲突，语义互补。
- 无代码 / CMake / 测试改动——ctest 不受影响，默认 build 保持绿。

**License impact.** 无。纯文档。

**Registration.** 无注册点变更。
- `docs/ARCHITECTURE_GRAPH.md` 一处新 section 插入。
- 无代码 / 无 schema / 无 CMake / 无 API。
