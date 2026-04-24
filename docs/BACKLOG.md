# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M4 主线 / 跨 milestone debt）

- **soundtouch-integration** — M4 exit criterion "SoundTouch 集成，支持变速不变调" 的起步。`grep -rn 'SoundTouch\|soundtouch' src include` 空（只有 `docs/ARCHITECTURE.md:89` 白名单声明 SoundTouch LGPL-2.1 Phase 4 time-stretch）。**方向：** (1) 根 `CMakeLists.txt` 新 `ME_WITH_SOUNDTOUCH` option (default ON 等 OCIO 节奏)。(2) `src/CMakeLists.txt` 加 `FetchContent_Declare(soundtouch GIT_REPOSITORY https://codeberg.org/soundtouch/soundtouch.git GIT_TAG <pin>)`；关 SOUNDTOUCH_DLL / 测试 / 示例 build。(3) `src/audio/tempo.{hpp,cpp}` 新 `me::audio::TempoStretcher` 薄 wrapper 对接 SoundTouch `SoundTouch::putSamples / receiveSamples / setTempo / setPitchSemiTones` API。(4) tests/test_tempo.cpp：输入 44.1k 1s 正弦波，tempo=2.0 → 输出 0.5s，FFT 主峰仍在同一频率（变速不变调）。Milestone §M4，Rubric §5.3。
- **vfr-av-sync** — M4 exit criterion "VFR 输入 + 分数帧率输出下 A/V 漂移 < 1 ms / 小时"。`src/orchestrator/reencode_segment.hpp:64` 注释承诺 "output even from VFR inputs, which is standard re-encode" 但没有漂移测量。`grep -rn 'VFR\|variable.*frame.*rate\|av.*sync' src tests` 基本空。现在 re-encode 把 VFR 当 CFR 处理（每输入帧一个输出帧，丢 / 重 frame），但不追踪 timestamps → 累积漂移可能大。**方向：** (1) `src/orchestrator/reencode_video.cpp` 改 frame-pulling loop 根据真实 `pts` vs 输出帧率调度：维护 `output_pts_accumulator`，每次输出帧时 `output_pts_accumulator += 1/output_fps`，从 demux 选 `pts >= output_pts_accumulator - 1/(2*fps)` 最近的 frame（drop / duplicate 以保持 wall-time 对齐）。(2) 对应 test：生成带 VFR mock input（frame_rate wobble ±10%）+ 分数帧率输出（30000/1001 = 29.97fps），跑 1hour-equivalent 长度的短 clip（其实 60s 即可按比例 assert < 1/60 ms 漂移），断言 output 音频 wave sample k 与 video frame k 的 PTS 差 < 16.67 ms 的 1/60 部分。Milestone §M4，Rubric §5.2。
- **audio-effect-chain-skeleton** — M4 exit criterion "Audio effect chain（gain / pan / 基础 EQ）". `grep 'AudioEffect\|class.*AudioEffect' src` 空。现有 `src/timeline/timeline_impl.hpp:183` 的 `Clip::gain_db` 是 per-clip 单一 gain；无 pan、EQ、chain 抽象。**方向：** 新 `src/audio/audio_effect.hpp` — 抽象 `AudioEffect::process(float* samples, int n_samples, int n_channels, int sample_rate)` 类 Effect 但针对音频。新 `AudioEffectChain`。首批 impl：`GainAudioEffect`（既有 gain_db 移过来）、`PanAudioEffect`（linear L/R 分配）、`LowpassAudioEffect`（单极 IIR filter 基础 EQ 起步）。Milestone §M4，Rubric §5.3。
- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4，Rubric §5.3。

## P2（未来，当前 milestone 不挤占）

- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M4-prep，Rubric §5.2。
- **debt-gpu-backend-noop-fallback-test** — `src/gpu/bgfx_gpu_backend.cpp:43-47` 的两阶段重试（Count → Noop）只在 auto-pick 失败时跑到 Noop；dev macOS 上 Metal 头less init 现在稳定成功（0×0 分辨率修复后），Noop 分支没 CI 覆盖。`grep 'Noop\|name.*bgfx-Noop' tests` 空。真到某个 driver 拒绝 headless init 才暴露这条分支的 bug。**方向：** 测试新增一条 strongly-force-Noop 路径——如提供 `ME_GPU_FORCE_NOOP=1` 环境变量让 BgfxGpuBackend 跳过 auto-pick 直接进 Noop；对应 test 断言 `name() == "bgfx-Noop"` 且 `available() == true`。也验证 dtor 在 Noop 初始化后能干净 shutdown。Milestone §M3-debt，Rubric §5.3。
