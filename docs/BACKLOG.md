# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **multi-track-compose-actual-composite** — M2 exit criterion "2+ video tracks 叠加, alpha / blend mode 正确" 的真正实装（最后一步）。`src/orchestrator/compose_sink.cpp:ComposeSink::process` 当前 delegation 到 `reencode_mux` 只传 `tracks[0]` 的 clip——**bottom-only** 输出，忽略上面所有 track。该 delegation 验证了端到端 sink pipeline，但语义上是 compose-degenerate case（其他 track 相当于 α=0 被丢）。剩余工作是在 process() 里替换 delegation 为 **per-output-frame 真 compose loop**：(1) encoder+mux setup（mirror reencode_pipeline 的 SharedEncState init，或抽 helper）；(2) per output frame timestamp T at tl.frame_rate 驱动循环；(3) `me::compose::active_clips_at(tl, T)` → N TrackActive；(4) per TrackActive 从对应 demux 抽 source frame at source_time（现有 reencode_segment decode 机制复用）；(5) `me::compose::frame_to_rgba8` 转 RGBA8；(6) `me::compose::alpha_over(dst, src, ..., opacity, BlendMode::Normal)` bottom→top 叠加（本 cycle 只走 Normal mode，blend mode 通过 Transform.opacity 连带）；(7) `me::compose::rgba8_to_frame` 转回 encoder YUV；(8) `encode_video_frame` 送 encoder；(9) audio：本 phase-1 仍从 tracks[0] 第一个 clip 抽 passthrough（multi-audio-mix 另在 `audio-mix-resample`）；(10) flush + trailer。需要新 2-asset fixture（两段源文件）或复用单源 ×2-track；新 `tests/test_compose_sink_e2e.cpp` 端到端验证产物非空 + 确定性 2 次 byte-identical。Milestone §M2，Rubric §5.1。
- **cross-dissolve-kernel** — M2 exit criterion "Cross-dissolve transition" 的实装部分。schema + IR 已在 `cross-dissolve-transition` cycle 就位（`src/timeline/timeline_impl.hpp` 的 `TransitionKind::CrossDissolve` + `struct Transition` + `Timeline::transitions`），但 `src/orchestrator/exporter.cpp` 的 non-empty transitions gate 直接 `ME_E_UNSUPPORTED "cross-dissolve / transitions not yet implemented"`——alpha 混合 + source handle / timing 对齐 + sink 重构未做。**方向：** 在 `src/compose/` 内（或新 `src/compose/transitions/`）实装 cross-dissolve 的逐像素 alpha mix：decode from-clip 尾段 N 秒 + to-clip 头段 N 秒 → linear alpha ramp (0→1) → 输出 mixed frames → encoder。决定 semantic：symmetric overlap（同时消耗两 clip 的 source handles）vs asymmetric（单边 mix 到固定帧）；前者要求 sourceRange 有 head/tail 预留。tests 覆盖 alpha ramp 数学 + e2e 确定性。Milestone §M2，Rubric §5.1。
- **audio-mix-resample** — M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 的承接部分。数学内核 + peak limiter 已在 `audio-mix-kernel` cycle 就位（`src/audio/mix.{hpp,cpp}`，15 case / 49 assertion）。剩余工作：libswresample 接线把 N 个 demux audio stream resample 到公共输出率（例如 48000 Hz float）+ channel layout 统一；per-clip `gainDb → linear` 已经有 `me::audio::db_to_linear` 可用；AudioMixScheduler 类似 `active_clips_at` 按 timeline 时间抽 per-track sample 窗口；重构 H264AacSink（或新 AudioCompSink）接收混音后的 AVFrame 而非从单 demux 抽 packet。2-track e2e determinism 测试。`src/orchestrator/exporter.cpp` 的 audio track gate 仍 UNSUPPORTED。Milestone §M2，Rubric §5.1。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
