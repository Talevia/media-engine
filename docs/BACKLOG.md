# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）

- **compose-determinism-regression-test** — M2 最后一个 exit criterion "确定性回归测试：同一 timeline 跑两次产物 byte-identical（软件路径）" 尚未覆盖 M2 features。`tests/test_determinism.cpp` 现有 4 TEST_CASE 都是 M1 passthrough / h264-videotoolbox 单轨；`grep -rn 'compose\|mixer\|transition\|cross_dissolve' tests/test_determinism.cpp` 返回空——ComposeSink 的 compose 路径（multi-track / Transform / cross-dissolve transition / audio mixer）无 byte-identical 回归覆盖。ComposeSink 走 h264_videotoolbox（HW，byte-identity run-to-run 不是合约保证）+ libavcodec aac。**方向：** 扩展 `test_determinism.cpp` 加一个 compose-path TEST_CASE：构造一个带 transition / audio mixer 的多轨 timeline，渲染 2 次到不同 out_path，slurp 两文件比 bytes。当 videotoolbox 不可用或 byte-mismatch 时依 `test_determinism` 既定 pattern skip with MESSAGE 而非 fail。进阶：考虑为 ComposeSink 加 software MPEG-4 Part 2 encoder option（libavcodec 内建 LGPL，已在 `tests/fixtures/gen_fixture.cpp` 验证 bit-exact）让 compose 路径真·软件确定性可测。Milestone §M2，Rubric §5.3。

## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **audio-only-timeline-support** — 纯 audio timeline（无 video track）目前 unsupported：`src/orchestrator/compose_sink.cpp:106` 硬取 `bottom_track_id = tl_.tracks[0].id`，`compose_sink.cpp:117` `if (bottom_clip_indices.empty()) return ME_E_INVALID_ARG "bottom track has no clips"`——若 track 0 是 Audio kind 且没 video clip，factory 直接失败。Exporter `route_through_compose` 接受 audio-only（`has_audio_tracks=true`），但到 compose factory 就跪。**方向：** ComposeSink 检测无 video track 的 audio-only 场景，skip 所有 video encoder / stream 配置，走 audio-only sink path（mixer → aac → mux without video stream）。可作 `AudioOnlySink` 独立 class 或 ComposeSink 的 variant。Milestone §M2，Rubric §5.1。
- **transition-to-clip-source-time-align** — cross-dissolve phase-1 已知限制（`docs/decisions/2026-04-23-cross-dissolve-transition-render-wire.md` "Phase-1 限制"）：transition window 内 to_clip decoder 顺序 pull，window_end 时解码器已消耗 `duration/2 × fps` 帧，接入 single-clip 区间后播放内容相对 schema 领先 `duration/2`。fixture 是慢变化 gradient 时不可见，真实 content 会产生不一致。**方向：** to_clip decoder 在 window_end 时 `avformat_seek_file` 回 `source_start + (window_end - to.time_start)` 对应的 frame；清 decoder 内部状态（`avcodec_flush_buffers`）。或在 transition 进入时让 to_clip 从 `source_start + (T_rel - window_start)` pre-roll。需考虑 seek 不 frame-accurate 的情况（I-frame seek + dummy pull 到目标 frame）。Milestone §M2，Rubric §5.1。
- **transition-with-transform** — cross-dissolve 现状：`src/orchestrator/compose_sink.cpp` Transition 分支 comment "Phase-1 limitations: No per-clip Transform applied to from/to during the transition window (identity assumed)"——两 endpoint 带 non-identity Transform 时 silent 忽略。**方向：** Transition 路径拆两条 affine_blit：from_rgba + to_rgba 各自经其 clip 的 Transform 生成 canvas-sized buffer，再 cross_dissolve 到 track_rgba。src dims != W×H 的 clip 也能参与 transition 了。Milestone §M2，Rubric §5.1。
- **debt-split-compose-sink-cpp** — `wc -l src/orchestrator/compose_sink.cpp` = 694 行（接近 P0 强制阈值 700）。单 TU 塞了 ComposeSink class + make_compose_sink factory + frame loop 三大职责，且 frame loop 内部 SingleClip / Transition 分支 ~200 LOC。**方向：** 拆 factory 到 `compose_sink_factory.cpp`、Transition branch 抽成 `compose_transition_step(ctx, from_td, to_td, t, W, H, ...)` 独立函数，或把整个 process() 方法里的 per-track 循环体抽成 free function。Milestone §M2 / Rubric 外·debt。
- **debt-split-timeline-loader-cpp** — `wc -l src/timeline/timeline_loader.cpp` = 476 行（P1 400-700 区间）。loader 主体 + parse_animated_static_number + parse_transform + parse_color_space + parse_rational 等 helper 全部同 TU。**方向：** 拆 parse helpers（低耦合的纯数据形状解析）到 `src/timeline/loader_helpers.cpp`。Milestone §M2 / Rubric 外·debt。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
