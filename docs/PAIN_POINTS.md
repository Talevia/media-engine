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

### 2026-04-23 · test-scaffold-doctest — FetchContent 依赖的 CMake 地板版本碎片化

doctest v2.4.11 的 `CMakeLists.txt` 声明 `cmake_minimum_required(VERSION 2.8)`，CMake 4.x 直接报错 "Compatibility with CMake < 3.5 has been removed"。解法是先 `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)` 再 `FetchContent_MakeAvailable`——这是 workaround 不是修复，上游一旦 bump 就要回头删。nlohmann/json、taskflow 未来任一依赖踩同样的地板，本项目就会反复在每次 FetchContent 处粘一坨 policy 设置。**方向：** 建一个 `cmake/fetchcontent_policy.cmake` 集中把所有第三方 policy workaround 收口，`tests/` 和 `src/` 都 `include()`；或等 doctest 升级后整体删掉这行。

### 2026-04-23 · test-scaffold-doctest — 测试 schema rejection 靠手撸 string::replace

`test_timeline_schema.cpp` 里要测"schemaVersion=2 被拒"、"多 clip 被拒"、"有 effects 被拒"——每个 case 都把常量 JSON 字符串拷一份、在里面 `find + replace`。维护成本主要在"JSON 里某个字段写法一改，几条测试的 `find` 就同时挂"。**方向：** 可以建一个极简的 timeline builder helper（`TimelineBuilder::minimal_valid().with_schema_version(2).build()`），返回 string。但现在只有 5 条 mutation test，引入 builder 收益有限；到 schema v2 migration / multi-clip 一落地，mutation 矩阵起码 15 条，到那时抽。现在记下"手撸 find+replace 不可扩展"这个触发点。

### 2026-04-23 · thumbnail-impl — `Thumbnailer` 类签名与 C API 口径不对齐

`src/orchestrator/thumbnailer.{hpp,cpp}` 按 "timeline → 某一时刻的缩略图" 设计，构造函数吃一个 `shared_ptr<const Timeline>`；但公共 C API `me_thumbnail_png(engine, uri, time, ...)` 的入参是单纯 URI，不涉及 timeline。结果 `src/api/thumbnail.cpp` 直接在 extern "C" 里写完整的 demux+decode+sws+PNG-encode 管线，完全**绕过** orchestrator 那个类——那个类自 M0 就是 stub 到现在，本轮新管线也没让它前进一步。**方向：** 两种角色实际是两条路（"asset 级 thumbnail" = 无 timeline 语境；"composition 级 thumbnail" = 走 timeline 合成后取帧）。应该把 `Thumbnailer` 重命名成 `CompositionThumbnailer`，另建一个 `AssetThumbnailer`（或直接让 C API 走 demux kernel → 第一帧 → encode 这条 graph 路径），避免一个类两副面孔。本轮不改，记录。

### 2026-04-23 · thumbnail-impl — 三份 AV* RAII unique_ptr deleter 在 src/ 里复制粘贴

`src/api/probe.cpp`、`src/orchestrator/reencode_pipeline.cpp`、`src/api/thumbnail.cpp` 各自独立声明了 `CodecCtxDel / FrameDel / PacketDel / SwsDel`（+ 有的加 SwrDel）的 unique_ptr deleter 组。内容一字不差、只是 namespace 括号包一下。每加一个用 FFmpeg 的模块就要复制同一份。**方向：** 在 `src/io/ffmpeg_raii.hpp`（新文件）集中声明这组 deleter 和 alias（`AvCodecCtxPtr / AvFramePtr / AvPacketPtr / AvSwsPtr / AvSwrPtr`），让三处引用一个 header。本轮不改（会把本 commit 偏离 plan），下一轮 debt cycle 抽。

### 2026-04-23 · multi-clip-single-track — `av_interleaved_write_frame` 清空 pkt 后再读字段是坑

被这个坑了一个小时：我在 `av_interleaved_write_frame` 返回 0 之后读 `pkt->pts + pkt->duration` 来更新 per-stream `last_end_out_tb`，结果永远是 `AV_NOPTS_VALUE + 0` —— FFmpeg 文档清楚写了"takes ownership of the packet reference and resets pkt"，但调用点在非常典型的 stream-copy 循环里，很容易错过。调试路径也反直觉：ffprobe 报 non-monotonic DTS，我以为是 offset 公式 bug，其实是 offset 永远没在推进。**方向：** 把 mux write 封一层 `write_and_track(pkt, last_end_out_tb, mapped)`，把 snapshot → write → commit 一起收口；或者未来的 `io::MuxContext` RAII wrapper（参见 2026-04-22 的 PAIN_POINTS 条目）自带这套簿记。多处手写 snapshot pattern 每处都容易再踩一次。

### 2026-04-23 · multi-clip-single-track — Timeline 的 `time_offset` 传进 passthrough 后被忽略

`PassthroughSegment::time_offset` 当前是 advisory —— 实际的输出 PTS 由"接着前一段的 DTS 走"决定（按 libavformat concat demuxer 的习惯）。这对 phase-1"contiguous clips no gaps"的场景是对的，但 schema v2 要加 gaps / silence（`timeRange.start > prior_end`）时，这个字段就必须被认真读入。**方向：** 等 schema 允许 gaps 再动。当下：struct 上那个字段实际是 dead code，容易误导读者以为在用。（没有修，因为 phase-1 不支持 gaps；但文档 + struct comment 都已说明，降低读代码时的困惑。）
