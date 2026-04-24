# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）


## P2（未来，当前 milestone 不挤占）

- **text-clip-color-animation** — `src/timeline/timeline_impl.hpp:167` `TextClipParams::color` 是 `std::string`；不是 AnimatedNumber / AnimatedString。`font_size / x / y` 都 animated，`color` 静态不对称。**方向：** 设计 typed animated-color primitive (`AnimatedColor` — RGBA × AnimatedNumber 四分量 or `{keyframes: [{t, color_hex}]}` 解析），让 TextClipParams.color 支持动画。TextRenderer::render 按 T 取当前颜色。Milestone §M5-debt (cross)，Rubric §5.2。
- **subtitle-clip-file-uri** — `src/timeline/timeline_impl.hpp:178` `SubtitleClipParams { content, codepage }`；cycle 30 scope 决策走 inline-only（BACKLOG 原 bullet 提到"或 file_uri"两路都可）。大 .ass 文件 inline 到 timeline JSON 膨胀。**方向：** 加 `SubtitleClipParams::file_uri` 字段；loader 接受 content 或 file_uri 二选一（二者同时填 → ME_E_PARSE）；compose_decode_loop 的 subtitle 分支在 `!file_uri.empty()` 时读文件 bytes 后再 load_from_memory。Milestone §M5-debt (cross)，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
