# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M5 主线 / 跨 milestone debt）

- **skia-integration** — M5 exit criterion "Skia 集成" 的起步。`grep -rn 'Skia\|skia' src include` 空（只有 `docs/ARCHITECTURE.md:85` 白名单声明 Skia BSD-3 Phase 5 text/vector）。**方向：** (1) 根 `CMakeLists.txt` 新 `ME_WITH_SKIA` option (default ON)。(2) `src/CMakeLists.txt` 加 `FetchContent_Declare(skia GIT_REPOSITORY ... GIT_TAG <stable-branch>)` 或先 `find_package(skia)`（Skia 的 CMake 是实验性的，可能需要 bazel 包装）。(3) 新 `src/text/skia_backend.{hpp,cpp}` — 薄 wrapper 对接 `SkCanvas::drawString` / `SkTextBlob`。(4) tests/test_skia_backend.cpp: 渲染 "Hello" 到 RGBA8 buffer，assert 至少有非零 alpha 像素（文本落在画布上）。Milestone §M5，Rubric §5.3。
- **text-clip-ir** — M5 exit criterion "Text clip (静态 + 动画字号 / 颜色 / 位置)" 的起步。`grep 'TextClip\|text_clip\|ClipType::Text' src include` 空——`src/timeline/timeline_impl.hpp:137-140` 的 `ClipType` 只有 Video / Audio。**方向：** (1) 扩 `ClipType` 加 `Text = 2`。(2) 新 `struct TextClipParams { std::string content; AnimatedNumber font_size; std::string color; AnimatedNumber x; AnimatedNumber y; }` — typed struct，非 map。(3) `me::Clip` 加 `std::optional<TextClipParams> text_params;` 字段。(4) `loader_helpers.cpp` 加 `parse_text_clip_params(const json&)`。(5) `timeline_loader.cpp` 识别 `"type": "text"` clip 类型 + 解析 text 专属字段（无 assetId，允许）。(6) tests/test_timeline_schema.cpp 加 text clip 正 / 负路径。Milestone §M5，Rubric §5.2。
- **text-clip-render-skia** — 依赖 `skia-integration` 和 `text-clip-ir`。`grep -rn 'render.*text\|text.*render' src` 空。Text clip IR 解析出来但 ComposeSink / 其他 render path 不画。**方向：** 新 `src/text/text_renderer.{hpp,cpp}` — 接受 `TextClipParams` 和一个 RGBA8 画布 (W, H)，在指定位置绘制文本。集成点：`src/orchestrator/compose_decode_loop.cpp` 的 per-frame loop 检测 `Clip::text_params`，调用 text_renderer 而不是 decode 视频帧。Milestone §M5，Rubric §5.3。
- **libass-subtitles** — M5 exit criterion "libass 字幕 track". `grep -rn 'libass\|ASS_\|ass_library' src include cmake` 空。**方向：** (1) 根 `CMakeLists.txt` 新 `ME_WITH_LIBASS` option。(2) `src/CMakeLists.txt` `find_package(libass)` 或 FetchContent (libass 非 CMake; 可能需要 pkg-config)。(3) 新 `src/text/subtitle_renderer.{hpp,cpp}` 对接 `ass_library_init` / `ass_read_file` / `ass_render_frame`。(4) 扩 timeline schema 加 `subtitle` track kind（类似 video/audio）。(5) Test: 加载 sample .ass 字幕文件，渲染单帧，assert 输出 RGBA 有非零像素。Milestone §M5，Rubric §5.3。
- **font-fallback-cjk-emoji** — M5 exit criterion "CJK + emoji + 字体 fallback 正确". `grep -rn 'fontconfig\|FcPattern\|FcConfig\|CoreText' src include` 空。Skia 集成后，当请求字体没有 CJK 字形或 emoji 时要自动 fallback。**方向：** 新 `src/text/font_resolver.{hpp,cpp}` — 平台分支：macOS 用 CoreText 的 `CTFontCreateForString` 自动 fallback；Linux 用 fontconfig 的 `FcPatternFormat`。Skia 的 `SkFontMgr::matchFamilyStyleCharacter` 是 API 前端。Test: 渲染 "hello 你好 👋" 到画布，assert 三段都有非零像素。Milestone §M5，Rubric §5.3。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M5-debt (cross)，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M5-debt (cross)，Rubric §5.2。
- **debt-gpu-backend-noop-fallback-test** — `src/gpu/bgfx_gpu_backend.cpp:43-47` 的两阶段重试（Count → Noop）只在 auto-pick 失败时跑到 Noop；dev macOS 上 Metal 头less init 现在稳定成功（0×0 分辨率修复后），Noop 分支没 CI 覆盖。`grep 'Noop\|name.*bgfx-Noop' tests` 空。真到某个 driver 拒绝 headless init 才暴露这条分支的 bug。**方向：** 测试新增一条 strongly-force-Noop 路径——如提供 `ME_GPU_FORCE_NOOP=1` 环境变量让 BgfxGpuBackend 跳过 auto-pick 直接进 Noop；对应 test 断言 `name() == "bgfx-Noop"` 且 `available() == true`。也验证 dtor 在 Noop 初始化后能干净 shutdown。Milestone §M3-debt，Rubric §5.3。
- **vfr-drift-1h-bench** — vfr-av-sync (0d0094b) 的 helper + push_video_frame fix 把 VFR 源 PTS 保留到输出，已 unit-test (`test_reencode_pts_remap.cpp`, 117 assertions)。但"1 ms / 小时"阈值没有端到端 fixture 证据。**方向：** 新 `bench/bench_vfr_av_sync.cpp` — 生成 synthetic VFR input（libav API 直接构造 AVPacket with irregular pts），跑过 reencode，ffprobe output video+audio streams，比较最后帧的 video pts (scaled) vs audio pts (scaled)，assert |diff| / duration < 1e-6 (= 1 ms / 1000s = 1 ms / hour scaled down)。配置 `ME_BUILD_BENCH=ON`. Milestone §M5-debt (cross)，Rubric §5.2。
