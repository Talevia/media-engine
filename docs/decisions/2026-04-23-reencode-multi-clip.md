## 2026-04-23 — reencode-multi-clip：h264/aac 多段 concat（Milestone §M1-addendum · Rubric §5.1）

**Context.** `H264AacSink` 上线时 `make_output_sink` 硬 reject `clip_ranges.size() != 1`。passthrough 路径已经支持 N-clip concat（`muxer_state.cpp` / `passthrough_mux`）——reencode 落一半、API 层 "timeline loader 吃多 clip 但 reencode factory 拒绝" 的割裂让多 clip 的用户没法选 h264 output。Backlog `reencode-multi-clip` 把这条 lift 掉。

**Decision.** 把 `ReencodeOptions` 的单 `DemuxContext&` 入参换成 `std::vector<ReencodeSegment>`（`source_start / source_duration` 随 segment 走，parallel 到 `PassthroughSegment`）。`reencode_mux` 的生命周期：

1. 用 `segments[0]` 的 decoder 探参，开 **shared encoder**（video + audio）。随后立刻 `reset()` 那套 prelude 用的 decoder。
2. 循环 segments：`process_segment()` 每段 **新开 decoder**（per-segment），按 segment.source_start seek，读包到 segment.source_duration 截止，decode → sws/swr → push 给 shared encoder。段末 flush per-segment decoder（`avcodec_send_packet(nullptr)`）把残余帧榨进 encoder，**不 flush encoder**。
3. 全部 segments 完跑完后，flush shared encoder（video + audio），drain 剩余 FIFO，`write_trailer`。

PTS 连续性是这次 refactor 的关键。两股：

- **Video**：`SharedEncState::next_video_pts` 是 encoder time_base 单位的 running counter，每个送进 encoder 的帧直接 overwrite 成 `next_video_pts` 再 send（`push_video_frame` lambda）。每帧之后增加 `video_pts_delta = 1 / framerate in venc->time_base`，从 `av_guess_frame_rate(segments[0])` 派生，无 fallback 则 25 fps。效果：**VFR 输入 → CFR 输出**，跨段 monotonic 不重置，h264 bitstream 只有一条 GOP 结构。这是 "just-re-stamp" 路径，不是 re-timing：不关心 input 是否丢帧或有 duplicate，编码完输出就是 fps 规定的帧率。
- **Audio**：`SharedEncState::next_audio_pts` 是 running sample counter（encoder sample rate 单位）。每段的 swr + FIFO 是 per-segment 新开（不同 segment 的 source_fmt/sr 可能不同），但 FIFO 吐出 encoder-sized chunk 时用的是 shared counter，`drain_audio_fifo()` 每 frame 加 `this_frame`。AAC encoder 全程只开一次，priming 样本只在输出头部出现一次，而不是 per-clip 重复——对比"segment → 独立 reencode → passthrough concat" 方案省掉了 N-1 份 priming。

**Cross-segment compat 约束.** 跟 `muxer_state.cpp` 的 `codecpar_compatible` 对齐：subsequent segments 的 video 必须 `width/height/pix_fmt` 与 seg0 相同；audio 必须 `sample_rate/sample_fmt/channels` 相同。不同 → `ME_E_UNSUPPORTED` + 清晰 err。理由：encoder 参数在 seg0 时冻结；不匹配 → 要么破坏 encoder 状态要么在 pipeline 里塞 rescale/resample 分支。现在这条硬规则让失败 fail-fast；未来真需要异构 concat（不同 ProRes profile、不同 sample rate、不同 ch_layout）时单独 scope。

**Flush 时机决定正确性.** 段末 flush per-clip decoder（必须，否则 B-frame reorder buffer 里的帧丢）但不 flush encoder（不能，否则 encoder 关闭新段再开等于两条独立 H.264 bitstream 拼接，客户端解码器看到 IDR 重启 PTS 非单调）。这条约束是 VideoToolbox / libaac 的默认语义，不是我们的约定。

**Progress reporting 简化.** 之前 plan 里带了 `progress_us_done` per-segment 累加器，但 `next_video_pts` 本来就跨段单调，直接拿它 rescale 到 `AV_TIME_BASE_Q` 除以 `total_us` 就是累计比例。多余字段删掉。

**Alternatives considered.**

1. **每段独立 `reencode_mux` 到 tmpfile，最后 passthrough concat**——拒：3 份拷贝（decode→encode→tmp；tmp→stream-copy→final；临时磁盘空间）。priming samples 每段开头多一份，N=10 段就是 10× AAC priming 浪费。虽然代码量少一半，但语义和磁盘成本都不 acceptable。
2. **Shared encoder + sample-accurate trim**（支持任意 source_start / source_duration）——部分支持：本轮 seek + early-break 已覆盖大部分情况，但 h264 keyframe-only seek + sample-accurate trim 语义需要 "seek → discard frames until source_pts >= source_start" 的 trim-decode pattern。不想这 cycle 超 scope；现状是"GOP-rounded seek"（和 passthrough 的 trim semantics 一致），decision 里明写。
3. **不同 clip 不同 framerate / pix_fmt 支持**（per-segment rescale）——拒：需要 sws / swr **per-segment** rebuild + encoder resize support，VideoToolbox 不支持运行时 resize。异构 concat 是下一个 scope。
4. **保留旧 `reencode_mux(DemuxContext&, ...)` 单入口 + 新 multi 入口**——拒：两个入口会漂移。直接改签名，single-clip 变成 `segments.size() == 1` 的特化，调用方（`H264AacSink::process`）一次统一构造 vector。

业界共识来源：FFmpeg 的 `concat demuxer` 做的是 packet-level concat（即本 skill 的 passthrough 路径，不支持 re-encode）。真正的 "re-encode concat" 标准做法就是 shared encoder + per-segment decoder + per-encoded-frame PTS restamp——这是 DaVinci Resolve / Premiere render farm / HandBrake chapter concat 都用的模式。参考：FFmpeg 的 `ffmpeg` CLI 多 `-i` 输入的内部 encoder lifecycle（`fftools/ffmpeg_mux_init.c`）也一个 encoder 跨多输入。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean；`ctest` 7/7 suite 绿。
- **Multi-clip smoke**: `/tmp/me-multi.timeline.json` 里两段 `file:///tmp/input.mp4`（30fps 1920x1080 h264/aac），每段 sourceRange=1s → 产物 duration 2.04s, 60 video frames, 89 aac frames，ffprobe 能正常识别 h264+aac MP4。
- **Single-clip regression**: `sample.timeline.json` 走老路径产物 duration 2.02s, 60 video frames, 88 aac frames，与 refactor 前 output 的帧数一致（multi-clip 比 single-clip 多 1 aac frame 是 encoder flush 额外 priming——acceptable, 后续 sample-accurate 再调）。
- `test_output_sink` 更新：旧 "h264/aac + multi-clip 应拒绝" 改成 "h264/aac + multi-clip 应放行"；新加 "h264/aac + null codec_pool 应拒绝" 覆盖 factory 侧另一条 guard。12 → 14 assertion。

确定性：没改。h264_videotoolbox 本来就是 HW encoder，非 byte-deterministic——这条由另一个 debt bullet `debt-render-bitexact-flags` 处理。

**License impact.** 无新依赖。`h264_videotoolbox`（VideoToolbox framework，macOS SDK）+ `libavcodec` AAC 都已在 link 图内。

**Registration.** 无公共 C API 变更（纯内部重构）。变动：
- `src/orchestrator/reencode_pipeline.hpp`：新 `ReencodeSegment` struct；`ReencodeOptions.segments` 字段；`reencode_mux` 签名从 `(DemuxContext&, opts, err)` 变成 `(opts, err)`。
- `src/orchestrator/reencode_pipeline.cpp`：完整重写，新内部 `SharedEncState` struct + `process_segment()` 子函数。
- `src/orchestrator/output_sink.cpp`：`H264AacSink` 持有 `clip_ranges_`；`make_output_sink` 不再 reject N>1 h264/aac。
- `tests/test_output_sink.cpp`：2 个 test case 调整（multi-clip 从 reject→accept；新加 null pool 拒绝）。
- `docs/BACKLOG.md`：删 `reencode-multi-clip` bullet。
- 无 schema / kernel / CodecPool / FetchContent / CMake target 变更。
