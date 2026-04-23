# Architecture pain points — running log

Observations recorded **during** `iterate-gap` cycles about where the current architecture made the task harder than it should be. Not a todo list (that's `BACKLOG.md`), not a decision record (that's `docs/decisions/`). These are informal notes for future refactor cycles to mine.

**Format.** One entry per pain point, appended at the bottom:

```
### <YYYY-MM-DD> · <cycle-slug> — <short title>

<2–4 sentences: what was awkward, where it bit, rough direction if known>
```

No rewriting, no reordering — this is a timeline of impressions.

---

### 2026-04-22 · reencode-h264-videotoolbox — Exporter 的"specialization 链"在第二条就开始疼

`Exporter::export_to` 现在靠 `is_passthrough_spec` / `is_h264_aac_spec` 两条 if-else 分派到 `passthrough_mux` / `reencode_mux`。每加一种 output 形态（prores、hevc、opus）都得在 `export_to` 里 capture 更多 lambda 变量、加一个新的分支、在 worker thread lambda 里再 if-else。**方向：** 把 mux/encode 一侧抽成一个 `OutputSink` interface（virtual `process(DemuxContext&, opts)`），按 spec 选一个具体实现注入 worker thread，`export_to` 只管调度；与 codec registry / TaskKindId 的未来"每 codec 一个 kind"模型同构。现在是两个分支，再多一个就该动手了。

### 2026-04-22 · reencode-h264-videotoolbox — Graph 现在只是 demux 的门面

Passthrough 和 reencode 都走 `build_passthrough_graph()` → await `DemuxContext` → 再由 orchestrator 接手同步流式工作。graph/scheduler 在 M1 的实际作用止于"开一个文件拿一个 shared_ptr"。`reencode_mux` 自己做 decode→encode→mux 的所有事，没有走 graph per-frame 路径（per-frame 决定论 + cache key 都不适用到 decoder 的 stateful streaming）。**方向：** 承认"整段 re-encode 是一个 stateful orchestrator job，不是 graph eval"是正确的架构选择——但这意味着 M4-M6 frame server 上线后会出现两种执行模型并存：(a) graph per-frame (preview / thumbnail)、(b) orchestrator streaming (export)。需要在 ARCHITECTURE_GRAPH.md 里显式写进来，不要假装只有一种。

### 2026-04-22 · reencode-h264-videotoolbox — AVFormatContext 的 RAII 一直没抽出来

`passthrough_mux.cpp` 和 `reencode_pipeline.cpp` 都手写了"alloc_output_context2 → avio_open → write_header → ... → avio_closep → free_context"这一套清理链，且各自把这套清理代码复制在 ~3 个 error 路径上。`demux_context.hpp` 已经为 input 侧做了 RAII wrapper（`io::DemuxContext`），**output 侧却没有对应的 `io::MuxContext`**。**方向：** 把 output AVFormatContext + avio_open 状态包成一个 `io::MuxContext` RAII 类型（跟 DemuxContext 对称），让 `passthrough_mux` / `reencode_mux` 只写"填 MuxContext、调 write_frame"。建一条 debt bullet 或下一 cycle 顺手抽。

### 2026-04-22 · reencode-h264-videotoolbox — `me_output_spec_t` 用 `const char*` 做 codec 选择

`video_codec = "h264"`、`audio_codec = "aac"` 是字符串——好处是 ABI 稳、C 友好；坏处是 `is_passthrough_spec` / `is_h264_aac_spec` 这种分支要靠 `strcmp` + 每加一种就要维护一个新的 helper。更痛的是 `spec.video_bitrate_bps` 不区分是给 h264 用、还是给 ProRes 用，所有 codec-specific 选项都只能共用这一层 flat struct。**方向：** 未来某个 milestone 需要每 codec 一个 typed option struct（`me_h264_opts_t`、`me_aac_opts_t`），spec 里带个 union / tagged 指针。但现在跨 ABI 边界整 union 成本高，等 M3-M4 再动。
