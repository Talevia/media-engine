## 2026-04-22 — orchestrator-bootstrap（Milestone §M1 · Rubric §5.1）

Commit: `<this>`

**Context.** 前几个 commit 把 graph / task / scheduler / resource 四模块的底座建起来，`me_render_start` 仍直接调 `io::remux_passthrough`。需要把 C API 的三个 entry（render_start / render_frame / thumbnail_png）收口到 orchestrator 层，让后续 `refactor-passthrough-into-graph-exporter` 可以只动 orchestrator 内部、不再动 `src/api/`。

**Decision.** 新增 `src/orchestrator/`：

- **`Exporter(me_engine*, shared_ptr<const Timeline>)`**——真正干活的一个。`export_to(spec, cb, user, &job, &err)` 做 spec 校验（passthrough-only 暂时）→ 启动 worker thread → 在 thread 里跑 `io::remux_passthrough`。worker 失败时通过 engine 的 `set_error` 写错误。Job 是 opaque，`me_render_job_t` 一对一包它。内部持 `SegmentCache` 字段但 bootstrap 不调——未来 refactor 填。
- **`Previewer(me_engine*, shared_ptr<const Timeline>)`**——`frame_at(time, &out_frame)` 返 `ME_E_UNSUPPORTED`，等 compose kernel + frame server（M6）。
- **`Thumbnailer(me_engine*, shared_ptr<const Timeline>)`**——`png_at(time, max_w, max_h, &png, &size)` 返 `ME_E_UNSUPPORTED`，等 thumbnail-impl backlog。
- **`SegmentCache`**——`unordered_map<uint64_t boundary_hash, shared_ptr<graph::Graph>>` + mutex。三个 orchestrator 各持一份；暂不跨实例共享。
- `src/api/render.cpp` 重写：`me_render_start` 构造 `Exporter` 委托；`me_render_frame` 构造 `Previewer` 委托。`me_render_job` 从"直接持 thread + cancel + result"变成"持 `Exporter::Job` unique_ptr"。
- `src/api/thumbnail.cpp` 暂不动——C API me_thumbnail_png 当前接 URI（不是 Timeline），和 `Thumbnailer(Timeline)` 签名不匹配；等 thumbnail-impl 一起设计（届时会把 URI 包进一个单 clip Timeline 再走 Thumbnailer）。

Timeline ownership 在 C API boundary 的处理：`me_timeline_t` 持 `me::Timeline` by value。orchestrator 要 `shared_ptr<const Timeline>`，bootstrap 用 no-op deleter 的借用包装——`shared_ptr<const Timeline>(&h->tl, [](auto*){})`。真正的 shared 所有权（让 orchestrator 能在 timeline handle destroy 后继续跑）随 refactor-passthrough-into-graph-exporter 一起来。

**Alternatives considered.**

1. **把 passthrough 逻辑直接搬进 Exporter::export_to**（不再调 `io::remux_passthrough`）——被拒：本 commit 的目标是"C API → orchestrator"的桥接，不是把 remux 搬家；保留 `remux_passthrough` 让 refactor-passthrough 的 diff 更聚焦（"remux → demux kernel + MuxerState"）。
2. **Previewer / Thumbnailer 的 stub 返回真实 Future<FrameHandle>**（而不是 `me_status_t + out_frame`）——被拒：C API 目前用同步 out-param，用 Future 就得同时改 API 形态，混合改动不利于审查。Future-based 内部调用在 refactor-passthrough-into-graph-exporter 引入。
3. **Thumbnailer 接 URI 而非 Timeline**（匹配 `me_thumbnail_png`）——被拒：偏离 ARCHITECTURE_GRAPH.md 定的 orchestrator 形态（都对 Timeline 求值）；URI-only 场景在上层 wrap 成单 clip Timeline。
4. **让 Timeline 真 shared ownership**（me_timeline 持 `shared_ptr<Timeline>`）——被拒：改动更大，触达 timeline loader + 所有消费 API；本轮用 no-op deleter 包装简化过渡。`refactor-passthrough-into-graph-exporter` 或更后面再做。

**Coverage.** 全 clean rebuild 后：
- 01_passthrough（C API）→ Exporter → `io::remux_passthrough` 仍产出 30 帧 h264 MP4，bit-identical
- 02_graph_smoke：5/5 pass
- 03_timeline_segments：6/6 pass
- `git grep 'ME_E_UNSUPPORTED' src` 计数不升（仍是 5：probe_* 两处、thumbnail 一处、render_frame 一处、reencode 未实装一处——减一是把 render_frame 的 stub 从 `api/render.cpp` 搬到 `orchestrator/previewer.cpp`，总数不变，分布换了）

**License impact.** 无新依赖。

**注册点.** 无——orchestrator 不是 task kind，不需要在 task registry 注册。
