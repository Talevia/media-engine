# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **multi-track-compose-actual-composite** — M2 exit criterion "2+ video tracks 叠加, alpha / blend mode 正确" 的真正实装（最后一步）。`src/orchestrator/compose_sink.cpp:ComposeSink::process` 当前 delegation 到 `reencode_mux` 只传 `tracks[0]` 的 clip——bottom-only 输出。**encoder+mux setup helper 已经抽好**（`src/orchestrator/encoder_mux_setup.{hpp,cpp}` 的 `setup_h264_aac_encoder_mux`——`multi-track-compose-encoder-mux-helper` cycle）。剩余工作：在 process() 里用 helper 拿到 encoder + mux + SharedEncState，然后：(1) per track 开独立 video decoder（暂定 phase-1 限制 "每 track 恰好 1 clip"——1 clip/track 去掉 within-track clip 切换复杂度）；(2) per output frame timestamp T at tl.frame_rate 驱动循环；(3) `me::compose::active_clips_at(tl, T)` → N TrackActive（受每 track 1 clip 限制，≈ one entry per active track）；(4) per TrackActive 从对应 track decoder advance 一帧；(5) `me::compose::frame_to_rgba8` 每 decoded frame；(6) `me::compose::alpha_over(dst, src, ..., 1.0, BlendMode::Normal)` bottom→top 叠加；(7) `me::compose::rgba8_to_frame` 回 encoder YUV target frame；(8) `detail::encode_video_frame`；(9) audio：phase-1 仍从 `tracks[0]` 第一个 clip demux 抽 audio packet passthrough（multi-audio-mix 另在 `audio-mix-scheduler-wire`）；(10) flush encoders + mux trailer。新 `tests/test_compose_sink_e2e.cpp` 2-track 端到端测试——同源 fixture ×2 tracks → h264/aac → 字节确定性。Milestone §M2，Rubric §5.1。
- **cross-dissolve-transition-wire** — M2 exit criterion "Cross-dissolve transition" 的承接实装。像素 lerp 数学已在 `cross-dissolve-kernel` cycle 就位（`src/compose/cross_dissolve.{hpp,cpp}`，9 case / 28 assertion）。剩余工作：决定 semantic（symmetric overlap vs asymmetric mix-to-fixed-frame；前者要求 sourceRange 留 head/tail handle），在 render 路径里对 `Timeline::transitions` 的每个 Transition（from_clip_id / to_clip_id / duration）：(1) decode from-clip 尾段 `duration` 秒 + to-clip 头段 `duration` 秒；(2) per overlap frame 计算 t ∈ [0,1]；(3) 调 `cross_dissolve(out, from_frame, to_frame, ..., t)`；(4) 喂 encoder；(5) `src/orchestrator/exporter.cpp` 的 non-empty transitions gate 翻成调用 ComposeSink（或 transition-aware sink）；(6) e2e 测试。Milestone §M2，Rubric §5.1。
- **audio-mix-scheduler-wire** — M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 的最后两块（scheduler + sink）。kernel (`me::audio::mix_samples` / `peak_limiter` / `db_to_linear`) + resample wrapper (`me::audio::resample_to`，9 case / 177 assertion) 已全部就位。剩余：(1) 新 AudioMixScheduler 类按 tl.frame_rate 逐 audio frame 从 N audio-track demux 抽 sample 窗口，per-track 经 `resample_to` 转公共 rate/fmt/ch_layout，per-clip `gain_db → db_to_linear` 应用，`mix_samples` 相加，`peak_limiter` 压，产出统一 AVFrame 队列。(2) 重构 H264AacSink（或新 AudioCompSink）audio path 改成调 scheduler 而非从单 demux passthrough。(3) Exporter audio track gate 翻。(4) 2-track e2e determinism 测试（mono 50Hz + mono 100Hz → stereo 48k）。Milestone §M2，Rubric §5.1。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
