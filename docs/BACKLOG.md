# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **debt-split-loader-helpers-cpp** — `src/timeline/loader_helpers.cpp` 401 行（阈值 400，紧贴）。`parse_effect_spec` + `parse_text_clip_params` + `parse_subtitle_clip_params` + 6 个原子 parser。**方向：** 按形状拆 `loader_helpers_primitives.cpp`（as_rational/require/rational_eq/color table）+ `loader_helpers_animated.cpp`（parse_animated_number + parse_transform）+ `loader_helpers_clip_params.cpp`（text/subtitle/effect spec 三个 clip-level）。`loader_helpers.hpp` 保持一个入口。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-frame-pool-enforce-budget** — `src/resource/frame_pool.hpp:66` `pressure()` 恒返回 0.0；`src/api/engine.cpp:33` 读 `config.memory_cache_bytes` 传给 FramePool ctor，但 `frame_pool.hpp:63` 构造器只存 `budget_`；`acquire()` 没 budget 检查。配置项"生效"但实际 no-op。**方向：** FramePool::acquire 前检查 `current_bytes + spec.bytes() > budget_` → 触发 LRU 驱逐（拒绝 or free oldest FrameHandle）；`pressure()` 改为 `current / budget`；me_cache_stats.memory_bytes_limit 返回配置值。Milestone §M7-debt (cross)，Rubric §5.2。
- **timeline-schema-subtitleparams-docs** — `docs/TIMELINE_SCHEMA.md:89` 列了 `"subtitle"` track kind 但没对应 subtitleParams 文档段（`grep -n 'subtitleParams' docs/TIMELINE_SCHEMA.md` 空；textParams 从 line 148 起文档齐全）。cycle 30 刚落地实装，schema doc 滞后。**方向：** 仿 `textParams` 段（docs/TIMELINE_SCHEMA.md:148-170）加 `subtitleParams.content` (required, UTF-8 .ass/.srt) + `subtitleParams.codepage` (optional, iconv codepage) + 一个 minimal 例子 JSON。Milestone §M7-debt (cross)，Rubric §5.6。
- **transform-on-text-subtitle-clips** — `src/timeline/timeline_loader.cpp:255` loader `require(track_kind == me::TrackKind::Video, ME_E_PARSE, ".transform: not valid on audio clip ...")` 把 text / subtitle 轨道都拒绝 transform。但 Clip::transform 字段对所有 ClipType 都有意义（位置、缩放、不透明度）；compose_decode_loop 的 transform/opacity 代码已经统一走 `transform_clip_idx` 对 text/subtitle 通用。**方向：** loader 放宽为 `track_kind != Audio`（音频仍拒绝；视频/文本/字幕允许）；错误信息更新；新增 test case 验证 text clip 带 transform.opacity 0.5 半透明渲染。Milestone §M5-debt (cross)，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **debt-disk-cache-size-limit** — `src/resource/disk_cache.hpp:35` 注释：`No LRU / size cap in phase-1`；`disk_cache.hpp:45` `clear()` 只按 `.bin` 后缀扫描。长时间 scrubbing 下 cache 目录会无界增长。**方向：** DiskCache 加 LRU 驱逐：`put` 时检查 `current_disk_bytes + file_size > limit` → 按 mtime 升序移除最老的 `.bin` 直到腾出空间；`clear()` 重置计数。me_cache_stats.disk_bytes_limit 返回配置值。Milestone §M7-debt (cross)，Rubric §5.2。
- **text-clip-color-animation** — `src/timeline/timeline_impl.hpp:167` `TextClipParams::color` 是 `std::string`；不是 AnimatedNumber / AnimatedString。`font_size / x / y` 都 animated，`color` 静态不对称。**方向：** 设计 typed animated-color primitive (`AnimatedColor` — RGBA × AnimatedNumber 四分量 or `{keyframes: [{t, color_hex}]}` 解析），让 TextClipParams.color 支持动画。TextRenderer::render 按 T 取当前颜色。Milestone §M5-debt (cross)，Rubric §5.2。
- **subtitle-clip-file-uri** — `src/timeline/timeline_impl.hpp:178` `SubtitleClipParams { content, codepage }`；cycle 30 scope 决策走 inline-only（BACKLOG 原 bullet 提到"或 file_uri"两路都可）。大 .ass 文件 inline 到 timeline JSON 膨胀。**方向：** 加 `SubtitleClipParams::file_uri` 字段；loader 接受 content 或 file_uri 二选一（二者同时填 → ME_E_PARSE）；compose_decode_loop 的 subtitle 分支在 `!file_uri.empty()` 时读文件 bytes 后再 load_from_memory。Milestone §M5-debt (cross)，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
