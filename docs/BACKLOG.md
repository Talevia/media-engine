# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

---

## P0（必做，阻塞当前 milestone）

- **cache-stats-impl** — `me_cache_stats` 目前返回全 0 的 stub（`STUB:` 标记于 `src/api/cache.cpp`）；M1 exit criterion 明确要求 hit/miss/entry_count 非零。**方向：** 在 `FramePool` / `AssetHashCache` / `CodecPool` 里计数（bytes_used, entry_count, hit/miss counters），`me_cache_stats_t` 字段挂到真实数；stub 标记删除。Milestone §M1，Rubric §5.3。

## P1（强烈建议，M1 收尾或 M2 起步）

- **debt-ffmpeg-raii-shared-header** — `src/api/probe.cpp`、`src/api/thumbnail.cpp`、`src/orchestrator/reencode_pipeline.cpp` 各自独立声明了 `CodecCtxDel / FrameDel / PacketDel / SwsDel / SwrDel` 的 unique_ptr deleter 组，内容完全一致（PAIN_POINTS 已多次点过）。**方向：** 新增 `src/io/ffmpeg_raii.hpp`，把 deleter + `AvCodecCtxPtr / AvFramePtr / AvPacketPtr / AvSwsPtr / AvSwrPtr` alias 一次性定义好；三处复制粘贴的版本删除，引用新 header。Milestone §M1-debt，Rubric §5.2。
- **debt-io-mux-context-raii** — `passthrough_mux` 和 `reencode_mux` 手写了"alloc_output_context2 → avio_open → write_header → avio_closep → free_context"这套清理链，每条 error 路径都要复制一遍（PAIN_POINTS 2026-04-22）。**方向：** 新增 `src/io/mux_context.{hpp,cpp}`，镜像 `io::DemuxContext` 的 RAII 设计（ctor 打开、dtor 关闭），让两个 mux 函数只写"填 MuxContext、调 write_frame"。也顺带把 "av_interleaved_write_frame 清空 pkt 后再读字段" 的坑封进 `MuxContext::write_and_track()` 里（PAIN_POINTS 2026-04-23）。Milestone §M1-debt，Rubric §5.2。
- **debt-split-reencode-pipeline** — `src/orchestrator/reencode_pipeline.cpp` 已 615 行（scan-debt §1 阈值 400–700，默认 P1）；video encoder / audio encoder / fifo 三个子系统挤在一个 TU。**方向：** 切成 `reencode_video.{hpp,cpp}`（open_video_encoder + encode_video_frame + sws staging）和 `reencode_audio.{hpp,cpp}`（open_audio_encoder + encode_audio_frame + AVAudioFifo 簿记），顶层 `reencode_pipeline.cpp` 只留 orchestration。Milestone §M1-debt，Rubric §5.2。
- **timeline-asset-map** — `Timeline` IR 只有 `std::vector<Clip>`，没有 assets map；`contentHash` 只好挂在 `Clip` 上重复存储（PAIN_POINTS 2026-04-23）。**方向：** 新增 `me::Asset { uri, content_hash, colorSpace? }`，`Timeline` 里新增 `std::unordered_map<std::string, Asset> assets`，`Clip` 改为只带 `asset_id`。访问 uri 要经 `timeline.assets.at(asset_id)`。M2 multi-track 落地前必做。Milestone §M1 / §M2-prep，Rubric §5.1。
- **output-sink-interface** — `Exporter::export_to` 现有两个 if-else 分支派到 `passthrough_mux` / `reencode_mux`；加第三种（prores、hevc）要在 worker thread lambda 里再 capture 一圈变量 + 再加一个分支（PAIN_POINTS 2026-04-22）。**方向：** 抽 `class OutputSink { virtual me_status_t process(DemuxContext&, ...) = 0; }` 接口；两个现有实现包成 `PassthroughSink` / `H264AacSink`，按 spec 选一个注入 worker thread。与未来 codec registry 同构。Milestone §M2-prep，Rubric §5.2。
- **codec-pool-impl** — `me::resource::CodecPool` 目前是空壳（构造即用、无缓存）；M4 audio polish + 多 clip re-encode 都需要 AVCodecContext 按 codec_id+profile+size 复用。**方向：** 内部 `unordered_map<CodecKey, shared_ptr<AVCodecContext>>` + mutex；get-or-create API；on_release 把 encoder 放回池（但 decoder 因为 stateful 仍要新建）。`me_cache_stats.codec_ctx_count` 也从这里来。Milestone §M1-close / §M4-prep，Rubric §5.3。
- **debt-cmake-policy-centralize** — `FetchContent_MakeAvailable(doctest)` 前要 `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)`（PAIN_POINTS 2026-04-23）；nlohmann/taskflow 未来任意一个踩同样地板，每处都要复制。**方向：** 新增 `cmake/fetchcontent_policy.cmake` 集中收口，`tests/` + `src/` 的 FetchContent 调用前都 `include()` 一次；doctest 当前 workaround 归位到新 helper 里。Milestone §M1-debt，Rubric §5.2。
- **debt-test-timeline-builder-helper** — `test_timeline_schema.cpp` + `test_determinism.cpp` 都在代码里手写 "最小合法 timeline JSON"（PAIN_POINTS 2026-04-23 两次点过）；schema 字段任何改名都要在 N 个 test 同时改。**方向：** 新增 `tests/timeline_builder.hpp` 提供 `TimelineBuilder::minimal_video_clip(uri, dur_num, dur_den)` 之类 builder，返回 `std::string`。每个 test 改用 builder，消除"N 处 find+replace"的维护负担。Milestone §M1-debt，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **ocio-integration** — 暂无色彩管理。**方向：** OpenColorIO FetchContent，assets 的 colorSpace → 工作空间转换 → 输出空间。依赖 timeline-asset-map 先落（colorSpace 要挂在 Asset 上）。Milestone §M2，Rubric §5.1。
- **multi-track-video-compose** — 只支持单轨。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。依赖 timeline-asset-map（多 track 共享 asset）+ output-sink-interface（合成后走单一 encode path）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **debt-schema-version-migration-hook** — schema v1 rejection 只认 `== 1`，没有 v2 迁移预演。**方向：** loader 里留 `migrate(v_from, v_to)` 接口，即使只支持 v1 也显式走一遍 migration path，未来 v2 接入零改动。Milestone §M2，Rubric §5.1。
- **reencode-multi-clip** — reencode path 仍只支持 single clip；passthrough 已支持 concat。**方向：** 每 clip 重置 encoder GOP bookkeeping + AAC priming 在边界处理；同 codec 可复用 encoder ctx（可参考 codec-pool-impl），不同 codec 必须新开 encoder。Milestone §M1-addendum，Rubric §5.1。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转（PAIN_POINTS 2026-04-23）。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口；现在记录不做。Milestone §M6-prep，Rubric §5.2。
