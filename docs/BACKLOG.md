# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **timeline-schema-subtitle-file-uri-docs** — cycle 41 落地 `SubtitleClipParams::file_uri`（`src/timeline/timeline_ir_params.hpp:185`）但 `docs/TIMELINE_SCHEMA.md` 的 "### Subtitle clip" 段只提 `content` + `codepage`，没 fileUri。**方向：** schema doc 加 fileUri 字段说明 + XOR 互斥规则（exactly one of content / fileUri）+ file:// URI 约定 + 示例。Milestone §M7-debt (cross)，Rubric §5.6。
- **engine-config-disk-cache-bytes-docs** — cycle 39 append `me_engine_config_t.disk_cache_bytes` 到公共 header；`grep -n 'disk_cache_bytes\|memory_cache_bytes' docs/API.md docs/VISION.md` 空——`docs/API.md` 没讲这两字段的 budget 语义 / 0=unlimited 约定。**方向：** docs/API.md 加 me_engine_config_t field reference table（num_worker_threads / log_level / cache_dir / memory_cache_bytes / disk_cache_bytes），含 0 默认值 + 单位 + 驱逐行为描述。Milestone §M7-debt (cross)，Rubric §5.6。
- **debt-dual-disk-counter** — `src/api/cache.cpp:20` `dir_used_bytes()` 扫盘算 on-disk bytes；`cache.cpp:68` `out->disk_bytes_used = dir_used_bytes(engine->disk_cache->dir())` 走 filesystem 扫描。但 cycle 39 在 DiskCache 里加了 running `disk_bytes_` atomic (`disk_cache.hpp:134`)。两路计数器容易漂移（DiskCache 内部看到 N，cache.cpp 扫盘看到 M）。**方向：** `me_cache_stats` 改为走 `engine->disk_cache->disk_bytes_used()` + `disk_bytes_limit()`；删除 `dir_used_bytes` helper + 里面的扫盘。单元测试覆盖 put/invalidate/clear 后两值一致。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-dup-bezier-math** — `src/timeline/animated_number.cpp:40-76` 和 `src/timeline/animated_color.cpp:29-64` 各自拥有一份 `bezier_y_at_x` + `fraction` helpers。cycle 40 明确留为 debt（见该 commit body "Alternatives considered"）。现在两 call site → 值得抽。**方向：** 新 `src/timeline/animated_interp.{hpp,cpp}` 内部 header（`me::timeline::detail` namespace）暴露 `fraction(t, a, b)` + `bezier_y_at_x(...)`；两 TU include 并消去本地 copy。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-doc-rot-orchestrator** — `src/orchestrator/previewer.hpp:5` 注释 "frame_at() returns ME_E_UNSUPPORTED until compose kernels + frame server (M6) land"——cycle 26 早已实装；`previewer.hpp:25` 方法 doc "Currently returns ME_E_UNSUPPORTED; awaits compose kernels..." 同类 stale。`src/orchestrator/previewer.hpp:31-33` 三个成员变量 `[[maybe_unused]] engine_ / tl_ / graph_cache_`——实际上 engine_ / tl_ 已被 frame_at 使用 (`previewer.cpp:174,182`)。`src/orchestrator/exporter.hpp:9` 注释 "Non-passthrough codecs return ME_E_UNSUPPORTED for now"——h264 reencode 早就 wired。**方向：** 更新三处 header 注释反映实装现状；删除 previewer.hpp 上错位的 `[[maybe_unused]]` annotation（graph_cache_ 可保留）。Milestone §M7-debt (cross)，Rubric §5.6。
- **subtitle-file-uri-error-diagnosis** — `src/orchestrator/compose_decode_loop.cpp:163-185` subtitle file_uri 路径在文件打不开 / 读失败时 `bytes` 静默为空 → SubtitleRenderer.valid() 留 false → render 变 no-op。host 无从知道 fileUri 错了（last_error 空）。**方向：** compose_decode_loop 的 subtitle 分支在 file read 失败时通过 `ctx.shared` 回传 err 消息（类似 decoder path 的 pattern）；me_render_wait 能拿到 "subtitle file 'X' not readable" 诊断。单元测试 fileUri 指向不存在路径 → last_error 非空。Milestone §M7-debt (cross)，Rubric §5.2。
- **text-clip-multiline-word-wrap** — `src/text/skia_backend.cpp:72-112` `draw_string` 调 SkCanvas::drawString 是单行 API。长 `TextClipParams.content` 溢出 canvas 右边界（无换行、无 wrap）。常见 motion-graphics 需求：caption-style 两三行 subtitle-ish text。**方向：** TextClipParams 加 `max_width` + `line_height_multiplier` 可选字段；SkiaBackend 加 `draw_paragraph(text, x, y, font_size, color, max_width)` 使用 Skia 的 SkParagraph module（或手动 word-break + shapeText）；render() 在 max_width 有值时走新路径。单元测试 emoji + CJK 两行 word-wrap。Milestone §M5-debt (cross)，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
