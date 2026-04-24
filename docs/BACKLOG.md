# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **schema-docs-text-maxwidth** — cycle 50 (4677e21) 为 `TextClipParams` 加了 `max_width` + `line_height_multiplier` 字段；`grep -n 'maxWidth\|lineHeightMul\|max_width' docs/TIMELINE_SCHEMA.md` 空。schema doc 没记录新字段；hosts 只能读代码才知道 maxWidth 存在。**方向：** 在 `### Text clip` 段的字段列表里加 `textParams.maxWidth` (number, optional — positive pixel cap; absent = no wrap) + `textParams.lineHeightMultiplier` (number, optional, default 1.2 — multiplier of font_size for between-line advance)；附加一个两行 word-wrap 示例 JSON。Milestone §M7-debt (cross)，Rubric §5.6。
- **debt-dup-keyframe-walker** — `src/timeline/loader_helpers_animated.cpp:122-171` (parse_animated_number 的 keyframes 数组遍历) 和 `src/timeline/loader_helpers_clip_params.cpp:195-250` (parse_animated_color 同类遍历) 各自 ~50 行几乎相同的 schema validation（`contains("t/v/interp")` + interp 解析 + bezier cp 边界 + strict-sort-by-t）。value type 差异只有 `double` vs `std::array<uint8_t, 4>`。**方向：** 抽 `src/timeline/loader_helpers_animated.hpp` 内 `namespace detail` 模板 `parse_keyframes<Value, ValueParser>(const json&, where, value_parser)` — 接回调解析 `v`；`parse_animated_number` + `parse_animated_color` 共用。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-raw-me-e-unsupported-audit** — `tools/scan-debt.sh` 最新 run 显示 "Raw `return ME_E_UNSUPPORTED` returns: 15 / Marked stubs: 0"。15 个位置都是合法 error path，但没有 `STUB:` marker 或 header 注释说明"为什么这里拒绝"/"什么 bullet 会解除限制"。未来重构时难判断是 stub 还是 legit reject。**方向：** 逐个 grep `ME_E_UNSUPPORTED` 站点（见 scan-debt §2），加一行 comment 说明：(a) legit reject + 为什么（譬如 "non-h264 codec at encoder setup"）或 (b) STUB + bullet slug (如果是未实装)。目标：下一次 scan-debt 报告 "Raw = marked stubs"（两数相等）。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-scaffold-readmes-doc-rot** — `src/graph/README.md:9`, `src/scheduler/README.md:9`, `src/task/README.md:9` 各自写 "Scaffolded, impl incoming——本目录只有这个 README"。但 `ls src/{graph,scheduler,task}` 显示每目录都有 6-8 个真实文件：`graph.{hpp,cpp}` / `scheduler.{hpp,cpp}` / `task_kind.hpp` / `eval_instance.{hpp,cpp}` 等。README 多个 milestone 前就过时了。**方向：** 三个 README 改为实际描述的模块 role（graph = 纯数据 Node/Graph；task = kernel registry；scheduler = task runtime）+ 列当前 file 的职责 + 哪些 rubric 轴已覆盖。Milestone §M7-debt (cross)，Rubric §5.6。
- **bench-text-paragraph-perf** — cycle 50 (4677e21) 加 `SkiaBackend::draw_paragraph` + 两个 test case 验证正确性，但没 bench 测吞吐。`draw_paragraph` 的 greedy codepoint 算法是 O(n) measureText calls per line；未来 caption-heavy workflow 可能需要 O(n log n) 或 SkParagraph 替代。没 baseline bench 就没 regression 信号。**方向：** 新 `bench/bench_text_paragraph.cpp` — 生成 1000-codepoint CJK+emoji 混合 string，测 `draw_paragraph` 每秒能处理多少 frame；配 `ME_BUILD_BENCH=ON` ctest entry。阈值：60 fps / paragraph for 1000 codepoint content。Milestone §M5-debt (cross)，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
