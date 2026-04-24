# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **cross-dissolve-transition-render-wire** — M2 exit criterion "Cross-dissolve transition" 的 render integration。三个 prereq 已就位：kernel (`src/compose/cross_dissolve.{hpp,cpp}`, 9 case / 28 assertion)、scheduler (`src/compose/active_clips.{hpp,cpp}` 的 `active_transition_at`, 3 new case / 13 assertion)、以及 `me::Clip::id`（`cross-dissolve-active-transition-scheduler` cycle 添加）。剩余工作：(1) render 路径在每 output frame T 调 `active_transition_at(tl, ti, T)` 查活跃 transition；(2) 活跃时从 from_clip decoder pull 一帧 at source_time = `from.source_start + (from.time_duration - duration/2 + (T - window_start))` 附近（从 from 尾段取），从 to_clip decoder pull 一帧 at source_time = `to.source_start + (T - window_start)` 附近（从 to 头段取）；(3) `cross_dissolve(out_rgba, from_rgba, to_rgba, W, H, stride, t)` 混合；(4) 替代 active_clips_at 产出（transition 窗口内 transition 优先于独立 clip）；(5) `ComposeSink::process` / Exporter 的 transitions gate 翻。需要每 track 支持 decoder 跨 clip 切换或同时持 2 decoder（复杂度来源）。e2e 测试：2-clip + 1s cross-dissolve timeline 渲染，产物比无 transition 版本不同。Milestone §M2，Rubric §5.1。
- **audio-mix-scheduler-wire** — M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 的最后两块（scheduler + sink）。kernel (`me::audio::mix_samples` / `peak_limiter` / `db_to_linear`) + resample wrapper (`me::audio::resample_to`，9 case / 177 assertion) 已全部就位。剩余：(1) 新 AudioMixScheduler 类按 tl.frame_rate 逐 audio frame 从 N audio-track demux 抽 sample 窗口，per-track 经 `resample_to` 转公共 rate/fmt/ch_layout，per-clip `gain_db → db_to_linear` 应用，`mix_samples` 相加，`peak_limiter` 压，产出统一 AVFrame 队列。(2) 重构 H264AacSink（或新 AudioCompSink）audio path 改成调 scheduler 而非从单 demux passthrough。(3) Exporter audio track gate 翻。(4) 2-track e2e determinism 测试（mono 50Hz + mono 100Hz → stereo 48k）。Milestone §M2，Rubric §5.1。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
