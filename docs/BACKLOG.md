# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M5 主线 / 跨 milestone debt）

- **font-fallback-cjk-emoji** — M5 exit criterion "CJK + emoji + 字体 fallback 正确". `grep -rn 'fontconfig\|FcPattern\|FcConfig\|CoreText' src include` 空。Skia 集成后，当请求字体没有 CJK 字形或 emoji 时要自动 fallback。**方向：** 新 `src/text/font_resolver.{hpp,cpp}` — 平台分支：macOS 用 CoreText 的 `CTFontCreateForString` 自动 fallback；Linux 用 fontconfig 的 `FcPatternFormat`。Skia 的 `SkFontMgr::matchFamilyStyleCharacter` 是 API 前端。Test: 渲染 "hello 你好 👋" 到画布，assert 三段都有非零像素。Milestone §M5，Rubric §5.3。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M5-debt (cross)，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M5-debt (cross)，Rubric §5.2。
- **vfr-drift-1h-bench** — vfr-av-sync (0d0094b) 的 helper + push_video_frame fix 把 VFR 源 PTS 保留到输出，已 unit-test (`test_reencode_pts_remap.cpp`, 117 assertions)。但"1 ms / 小时"阈值没有端到端 fixture 证据。**方向：** 新 `bench/bench_vfr_av_sync.cpp` — 生成 synthetic VFR input（libav API 直接构造 AVPacket with irregular pts），跑过 reencode，ffprobe output video+audio streams，比较最后帧的 video pts (scaled) vs audio pts (scaled)，assert |diff| / duration < 1e-6 (= 1 ms / 1000s = 1 ms / hour scaled down)。配置 `ME_BUILD_BENCH=ON`. Milestone §M5-debt (cross)，Rubric §5.2。
