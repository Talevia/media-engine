# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **multi-track-compose-kernel** — M2 exit criterion "2+ video tracks 叠加, alpha + blend mode" 的实装部分。schema + IR 已在 `multi-track-video-compose` cycle 就位（`src/timeline/timeline_impl.hpp` 的 `Track` struct + `Clip::track_id` + `Timeline::tracks`），但 `src/orchestrator/exporter.cpp` 多轨 gate 直接 `ME_E_UNSUPPORTED "multi-track compose not yet implemented"`——compose 内核 + sink 重构未做。**方向：** 新 `src/compose/` 模块：alpha-over kernel（RGBA8 bottom + top + alpha → RGBA8 output，blend modes normal/multiply/screen）；重构 reencode path 让 sink 读 composited RGBA frame 而非直接 demux packet（新增 `ComposeSink` 或扩展 `H264AacSink` 支持 multi-demux input）；tests 覆盖 blend-mode 数学正确 + 2-track e2e 输出字节确定性。最大 follow-up bullet，估计 3–5 cycle。Milestone §M2，Rubric §5.1。
- **cross-dissolve-kernel** — M2 exit criterion "Cross-dissolve transition" 的实装部分。schema + IR 已在 `cross-dissolve-transition` cycle 就位（`src/timeline/timeline_impl.hpp` 的 `TransitionKind::CrossDissolve` + `struct Transition` + `Timeline::transitions`），但 `src/orchestrator/exporter.cpp` 的 non-empty transitions gate 直接 `ME_E_UNSUPPORTED "cross-dissolve / transitions not yet implemented"`——alpha 混合 + source handle / timing 对齐 + sink 重构未做。**方向：** 在 `src/compose/` 内（或新 `src/compose/transitions/`）实装 cross-dissolve 的逐像素 alpha mix：decode from-clip 尾段 N 秒 + to-clip 头段 N 秒 → linear alpha ramp (0→1) → 输出 mixed frames → encoder。决定 semantic：symmetric overlap（同时消耗两 clip 的 source handles）vs asymmetric（单边 mix 到固定帧）；前者要求 sourceRange 有 head/tail 预留。tests 覆盖 alpha ramp 数学 + e2e 确定性。Milestone §M2，Rubric §5.1。
- **ocio-colorspace-conversions** — M2 exit criterion "OpenColorIO 集成" 的实装部分。`src/color/ocio_pipeline.cpp` 里 `OcioPipeline::apply` 对 `src != dst` 全部 return `ME_E_UNSUPPORTED "non-identity colorspace conversion not yet implemented"`（由 `ocio-pipeline-enable` cycle 引入的 scope gate）。M2 exit criterion 要求 "支持 bt709/sRGB/linear" 之间的转换。**方向：** 基于 OCIO CG config `cg-config-v2.1.0_aces-v1.3_ocio-v2.3` 的 role map，实装 `me::ColorSpace` tags → OCIO role name（如 `Transfer::BT709 + Primaries::BT709 → "Rec.709"`、`Transfer::SRGB → "sRGB"`、`Transfer::Linear + Primaries::BT709 → "lin_rec709"`）的映射函数；构造 `OCIO::ColorSpaceTransform` + `CPUProcessor`；对 RGBA8 / RGB8 buffer 应用 processor（OCIO 有 packed-uint8 API 或走 float roundtrip）。tests：已知像素值 round-trip（bt709→linear→bt709 should be identity within 1 LSB）、sRGB encode gamma 曲线的几个 known-value check。Milestone §M2，Rubric §5.1。
- **audio-mix-kernel** — M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 的实装部分。schema + IR 已在 `audio-mix-two-track` cycle 就位（`src/timeline/timeline_impl.hpp` 的 `TrackKind::Audio` + `ClipType::Audio` + `Clip::gain_db`），但 `src/orchestrator/exporter.cpp` 的 audio track gate 直接 `ME_E_UNSUPPORTED "standalone audio tracks not yet implemented"`——混音内核 + sink 重构未做。**方向：** 新 `src/audio/mix.{hpp,cpp}`（或 `src/compose/audio_mix.*`）：libswresample 接线把每 audio source resample 到公共输出率（例如 48000 Hz S16）、per-clip gainDb 应用（10^(gain/20) 线性乘法）、逐 sample 相加、简单 soft-knee peak limiter（threshold 0.95，超过按 tanh 压）；重构 reencode path 让 H264AacSink / AudioOnlySink 接收混音后的 AVFrame 序列而非从 demux 直接抽 packet；tests 覆盖 mix 数学（两路 1.0 + -∞ → 1.0，limiter 输入 1.5 → 输出 ≤ 1.0）+ 2-track e2e 确定性。最大 follow-up bullet，估计 3–4 cycle。Milestone §M2，Rubric §5.1。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
