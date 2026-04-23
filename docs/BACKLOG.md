# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **compose-transform-affine-wire** — M2 exit criterion "Transform (静态) 端到端生效" 的 wiring 部分。kernel + inverse-matrix helper 已在 `compose-transform-affine-blit-kernel` cycle 就位（`src/compose/affine_blit.{hpp,cpp}`，8 case / 105 assertion）。剩余工作：在 `ComposeSink::process` 的 per-TrackActive loop 内，用 `compose_inverse_affine(clip.transform->translate_x, translate_y, scale_x, scale_y, rotation_deg, anchor_x, anchor_y, src_w, src_h)` 得 inverse matrix，用 `affine_blit(intermediate_rgba, dst_w, dst_h, ..., track_rgba, src_w, src_h, ..., inv)` 把 track frame 变换到 canvas-size 中间 buffer，再 `alpha_over(dst_rgba, intermediate, opacity, Normal)`。新 e2e 测试验证非 identity translate 产物 != identity 产物（e.g. file_size 对比）。关闭 M2 "Transform (静态)" exit criterion。Milestone §M2，Rubric §5.1。
- **cross-dissolve-transition-wire** — M2 exit criterion "Cross-dissolve transition" 的承接实装。像素 lerp 数学已在 `cross-dissolve-kernel` cycle 就位（`src/compose/cross_dissolve.{hpp,cpp}`，9 case / 28 assertion）。剩余工作：决定 semantic（symmetric overlap vs asymmetric mix-to-fixed-frame；前者要求 sourceRange 留 head/tail handle），在 render 路径里对 `Timeline::transitions` 的每个 Transition（from_clip_id / to_clip_id / duration）：(1) decode from-clip 尾段 `duration` 秒 + to-clip 头段 `duration` 秒；(2) per overlap frame 计算 t ∈ [0,1]；(3) 调 `cross_dissolve(out, from_frame, to_frame, ..., t)`；(4) 喂 encoder；(5) `src/orchestrator/exporter.cpp` 的 non-empty transitions gate 翻成调用 ComposeSink（或 transition-aware sink）；(6) e2e 测试。Milestone §M2，Rubric §5.1。
- **audio-mix-scheduler-wire** — M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 的最后两块（scheduler + sink）。kernel (`me::audio::mix_samples` / `peak_limiter` / `db_to_linear`) + resample wrapper (`me::audio::resample_to`，9 case / 177 assertion) 已全部就位。剩余：(1) 新 AudioMixScheduler 类按 tl.frame_rate 逐 audio frame 从 N audio-track demux 抽 sample 窗口，per-track 经 `resample_to` 转公共 rate/fmt/ch_layout，per-clip `gain_db → db_to_linear` 应用，`mix_samples` 相加，`peak_limiter` 压，产出统一 AVFrame 队列。(2) 重构 H264AacSink（或新 AudioCompSink）audio path 改成调 scheduler 而非从单 demux passthrough。(3) Exporter audio track gate 翻。(4) 2-track e2e determinism 测试（mono 50Hz + mono 100Hz → stereo 48k）。Milestone §M2，Rubric §5.1。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
