# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **bgfx-integration-skeleton** — M3 exit criterion "bgfx 集成，macOS Metal 后端可渲染" 的起步。接口 skeleton 已就位（`src/gpu/gpu_backend.{hpp,cpp}` + `null_gpu_backend.hpp`，4 case / 204 assertion，由 `gpu-backend-skeleton` cycle 添加）—— `me::gpu::GpuBackend` 抽象类 + `NullGpuBackend` 默认 + `make_gpu_backend()` factory + 根 CMake `ME_WITH_GPU` option (默认 OFF, 占位)。**剩余工作：** (1) 根 `CMakeLists.txt` 在 `if(ME_WITH_GPU)` 下加 `FetchContent_Declare(bgfx)` 通过 `bkaradzic/bgfx.cmake` wrapper（bgfx 自家非 CMake 构建）。(2) 新 `src/gpu/bgfx_gpu_backend.{hpp,cpp}` 实装 `BgfxGpuBackend : GpuBackend` —— `bgfx::Init` w/ Metal renderer, `bgfx::setViewClear`, `bgfx::frame`, `bgfx::shutdown`；`available()` 返 true when init 成功。(3) `make_gpu_backend()` 改成 `#if ME_WITH_GPU → BgfxGpuBackend` 分支，else `NullGpuBackend`。(4) `me_engine` 持 `std::unique_ptr<GpuBackend>` 字段（engine create 时 `= make_gpu_backend()`）。(5) ARCHITECTURE.md 白名单 + version pin 落地（bgfx commit hash）。Skeleton scope 止于"bgfx init 成功 + clear a backbuffer + shutdown"——真实 effect render 走 `effect-gpu-*` 后续 bullet。Milestone §M3，Rubric §5.3。
- **transition-to-clip-source-time-align** — cross-dissolve phase-1 已知限制（`docs/decisions/2026-04-23-cross-dissolve-transition-render-wire.md` "Phase-1 限制"）：transition window 内 to_clip decoder 顺序 pull，window_end 时解码器已消耗 `duration/2 × fps` 帧，接入 single-clip 区间后播放内容相对 schema 领先 `duration/2`。fixture 是慢变化 gradient 时不可见，真实 content 会产生不一致。**方向：** to_clip decoder 在 window_end 时 `avformat_seek_file` 回 `source_start + (window_end - to.time_start)` 对应的 frame；清 decoder 内部状态（`avcodec_flush_buffers`）。或在 transition 进入时让 to_clip 从 `source_start + (T_rel - window_start)` pre-roll。需考虑 seek 不 frame-accurate 的情况（I-frame seek + dummy pull 到目标 frame）。Milestone §M2，Rubric §5.1。
- **transform-animated-support** — M3 exit criterion "所有 animated property 类型的插值正确" 已几乎全部落地：layer 1 (IR + 插值数学, `animated-number-primitive`)、layer 2 (JSON parse, `animated-number-loader`)、layer 3 Transform integration (`transform-animated-integration`: `me::Transform` 的 8 字段 `double → AnimatedNumber`，新 `TransformEvaluated` struct，parse_transform 用 parse_animated_number，ComposeSink + compose_transition_step 每帧调 `evaluate_at(T)`，test_timeline_schema 3 tests 迁移 + 替换 rejection test 为 parses-correctly)。**剩余工作**: (1) `me::Clip::gain_db` 从 `std::optional<double>` 迁到 `std::optional<AnimatedNumber>` + AudioMixer 每采样窗口 evaluate_at(T) —— 需要把 T 传进 mixer API，mixer 当前抽象无 T 参数。(2) E2e 测试带真 keyframes transform 跑 compose 管线渲染（单测已覆盖 parse + evaluate_at 正确性；e2e 只是 regression tripwire）。Milestone §M3，Rubric §5.1。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
