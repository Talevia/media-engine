# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M3 主线 / 跨 milestone debt）

- **effect-chain-gpu-pass-merge** — `src/effect/effect_chain.hpp:55-57` 的 `EffectChain::apply` 是裸 for 循环调用每个 Effect::apply，无合并。M3 exit criterion "EffectChain 能把连续 ≥ 2 个像素级 effect 合并成单 pass" 明文要求 GPU 路径合并。**方向：** 新 `me::effect::GpuEffectChain::compile()` — 检测同形 effect（color-correct + color-correct、或更一般地：所有 effect 标 `is_per_pixel() == true` 且读写样式兼容）并生成 fused fragment shader：把两个 effect 的 fragment main 串联、uniform buffer 合并、layout 重打包。Fallback: non-fuseable effects 走独立 pass。test: 2 个 color-correct chain 跑 profiler 确认只有 1 次 draw call。Milestone §M3，Rubric §5.1。
- **gpu-render-1080p60-bench** — `grep -rn 'benchmark\|1080p\|60fps' tests examples` 空。M3 exit criterion "1080p@60 可实时渲染带 3-5 个 GPU effect 的 timeline" 需要 bench harness + 性能闸。**方向：** 新 `bench/bench_gpu_compose.cpp`（build target `bench_gpu_compose` 只在 `ME_BUILD_BENCH=ON` 且 `ME_WITH_GPU=ON` 时开）。跑 N 帧 1080p × (color-correct + blur + lut) chain，`std::chrono::high_resolution_clock` 测端到端时间，断言 avg frame time < 16.67 ms（60 fps budget）。Build-time smoke：跑 10 帧即可（CI 时间预算）；release 跑 600 帧更稳。Milestone §M3，Rubric §5.3。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **debt-gpu-backend-noop-fallback-test** — `src/gpu/bgfx_gpu_backend.cpp:43-47` 的两阶段重试（Count → Noop）只在 auto-pick 失败时跑到 Noop；dev macOS 上 Metal 头less init 现在稳定成功（0×0 分辨率修复后），Noop 分支没 CI 覆盖。`grep 'Noop\|name.*bgfx-Noop' tests` 空。真到某个 driver 拒绝 headless init 才暴露这条分支的 bug。**方向：** 测试新增一条 strongly-force-Noop 路径——如提供 `ME_GPU_FORCE_NOOP=1` 环境变量让 BgfxGpuBackend 跳过 auto-pick 直接进 Noop；对应 test 断言 `name() == "bgfx-Noop"` 且 `available() == true`。也验证 dtor 在 Noop 初始化后能干净 shutdown。Milestone §M3-debt，Rubric §5.3。
