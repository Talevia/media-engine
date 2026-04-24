# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **cross-dissolve-transition-render-wire** — M2 exit criterion "Cross-dissolve transition" 的 render integration。五个 prereq 已就位：kernel (`src/compose/cross_dissolve.{hpp,cpp}`, 9 case / 28 assertion)、scheduler (`active_transition_at`, 3 case / 13 assertion)、`me::Clip::id`、precedence resolver (`frame_source_at` + `FrameSource`, 4 case / 18 assertion), 以及 **per-clip decoder indexing** in ComposeSink (`src/orchestrator/compose_sink.cpp:167` 的 `clip_decoders[clip_idx]`, 由 `cross-dissolve-compose-sink-clip-decoders` cycle 替代原 per-track 索引——decoder lookup 现在按 clip_idx 直接命中，两个 clip 的 decoder 可以同时存在, 通过 3 个现有 2-track compose e2e 测试作为 byte-identical regression cover)。剩余工作：(1) `ComposeSink::process` frame loop 改调 `frame_source_at(tl, ti, T)` 代替 `active_clips_at`（`src/orchestrator/compose_sink.cpp:231`）；(2) Transition kind 分支 pull 两个 decoder（`clip_decoders[fs.transition_from_clip_idx]` + `clip_decoders[fs.transition_to_clip_idx]`）；(3) `cross_dissolve(out_rgba, from_rgba, to_rgba, W, H, stride, fs.transition.t)` 混合；(4) `src/orchestrator/exporter.cpp:90` 的 transitions ME_E_UNSUPPORTED gate 翻。e2e 测试：2-clip + 1s cross-dissolve timeline 渲染，产物比无 transition 版本不同。Milestone §M2，Rubric §5.1。
- **audio-mix-scheduler-wire** — M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 的最后两块（scheduler + sink）。kernel (`me::audio::mix_samples` / `peak_limiter` / `db_to_linear`) + resample wrapper (`me::audio::resample_to`，9 case / 177 assertion) + **per-track feed** (`me::audio::AudioTrackFeed` + `open_audio_track_feed` + `pull_next_processed_audio_frame`, `src/audio/track_feed.{hpp,cpp}`, 8 case / 1288 assertion，由 `audio-mix-track-feed` cycle 添加——把一条 audio track 的 decode→resample→gain 绑成可独立测试的 primitive) 已全部就位。剩余：(1) 新 AudioMixer 类持 N 个 AudioTrackFeed，每 "mix 窗口" 从每 feed 拉 samples，经 `mix_samples` 相加、`peak_limiter` 压，产出统一 AVFrame 队列。(2) 重构 H264AacSink（或新 AudioCompSink）audio path 改成调 mixer 而非从单 demux passthrough。(3) Exporter audio track gate 翻。(4) 2-track e2e determinism 测试（mono 50Hz + mono 100Hz → stereo 48k）。Milestone §M2，Rubric §5.1。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
