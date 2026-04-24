# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **debt-split-test-timeline-schema-cpp** — `wc -l tests/test_timeline_schema.cpp` = 1535 行，远超 src 用的 400 行阈值。TEST_CASE 覆盖多个 schema 段（schemaVersion / assets / tracks / clips / transitions / effects / text / subtitle / animated-transform / ...）。一个 commit 修了 schema 加测试容易影响无关 TEST_CASE。**方向：** 按 schema 段落拆（`test_timeline_schema_top_level.cpp`, `test_timeline_schema_clips.cpp`, `test_timeline_schema_transitions.cpp`, `test_timeline_schema_animated.cpp`），共享 fixture 抽到 `tests/timeline_schema_fixtures.hpp`。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-split-test-compose-sink-e2e-cpp** — `wc -l tests/test_compose_sink_e2e.cpp` = 1136 行。16 个 TEST_CASE 覆盖多种 pipeline（2-track / transform / cross-dissolve / text / subtitle / fileUri / negative 控制 / audio-only）。同前 bullet 的组织债。**方向：** 按 pipeline 拆 `test_compose_e2e_video.cpp`、`test_compose_e2e_text.cpp`、`test_compose_e2e_subtitle.cpp`、`test_compose_e2e_audio.cpp`；保留共享 `EngineHandle / TimelineHandle / JobHandle` RAII 在 `tests/compose_e2e_handles.hpp`。Milestone §M7-debt (cross)，Rubric §5.3。
- **test-render-frame-concurrent-scrub** — `grep -rn 'me_render_frame.*thread\|concurrent.*render_frame' tests` 空。`me_render_frame` 创建 fresh Previewer 每次调用 (`src/api/render.cpp:95`)，理论上线程安全（DiskCache 有 mutex），但无 test 证实 UI-thread scrub 场景下并发 calls 行为。**方向：** 新 `tests/test_frame_server_concurrent.cpp` — spawn 4 线程，每线程 100 次 `me_render_frame` at 不同 time；assert all return ME_OK + frames non-null + DiskCache counters 增加。Milestone §M7-debt (cross)，Rubric §5.3。
- **bench-thumbnail-png-perf** — `grep -rn 'bench_thumbnail\|me_thumbnail_png' bench` 空。`me_thumbnail_png` 是 M1 最早实装的 C API 之一，没 regression 信号。Host use case（timeline scrubbing 列表生成缩略图）对延迟敏感。**方向：** 新 `bench/bench_thumbnail_png.cpp` — 用 determinism fixture，测 `me_thumbnail_png(uri, t=0.5s, 160x120)` 每次调用延迟；阈值：avg < 50 ms / thumbnail on dev box。Milestone §M7-debt (cross)，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
