# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）

- **debt-jni-thumbnail-validate-png** — `bindings/jni/CMakeLists.txt:add_test(NAME jni_thumbnail_smoke ...)` 跑 Thumbnail.java 写 ~15 KB 输出但无后续校验；`grep -rn 'PASS_REGULAR_EXPRESSION\|magic\|file -b' bindings/jni/CMakeLists.txt` 空。一次 nativeThumbnail regression 把 PNG 输出退化成空 byte[] / random bytes 都不会 trip ctest（exit 0 + 文件存在但内容垃圾）。**方向：** 仿 cycle 80 的 example_08 companion 模式：新 ctest `jni_thumbnail_smoke_validate` 跑 `bash -c 'head -c 8 <png> | od -An -tx1 | grep -q "89 50 4e 47"'` 验 PNG signature；DEPENDS jni_thumbnail_smoke。Milestone §M7-debt (coverage)，Rubric §5.3。
- **debt-jni-passthrough-validate-mp4** — 同上 shape — `jni_passthrough_smoke` 写 ~246 KB MP4 但无 ftyp 校验。**方向：** companion ctest 验 file(1) magic 或读 4-7 字节匹配 ASCII "ftyp"（`bash -c 'dd if=<mp4> bs=1 skip=4 count=4 status=none | grep -q ftyp'`）。Milestone §M7-debt (coverage)，Rubric §5.3。
- **debt-render-spec-arg-validation** — `me_render_start` 的 `me_output_spec_t.frame_rate.den == 0` 会触发 div-by-zero；`spec.width = 0` 行为未文档化；`grep -rn 'spec.frame_rate\|spec.width' tests/test_output_spec.cpp tests/test_render_*.cpp 2>/dev/null` 显示 test_compose_frame_convert.cpp:79 只测了 frame.width=0 (decode 端)。**方向：** test_output_spec.cpp 新 SUBCASE — `me_render_start(spec_with_zero_den)` 应返回 ME_E_INVALID_ARG 并写 last_error；同样验 width=0 / height=0。Milestone §M7-debt，Rubric §5.5。

## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **debt-split-reencode-segment-cpp** — `wc -l src/orchestrator/reencode_segment.cpp` = 375，repo 内最大 src 文件，距 §1a 阈值 25 行。下一个 reencode feature（per-segment audio mix or per-segment color override）几乎必触发 P0 split。**方向：** 走 cycle 72/73/81 的 _impl 模式 — 根据文件实际结构（先 grep 结构）抽 inner-loop body 或 setup helpers。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-jni-progress-trampoline-exception-test** — `bindings/jni/me_jni.cpp:74` `if (env->ExceptionCheck()) env->ExceptionClear();`；comment 写 "callback impls shouldn't throw, but if they do, swallow"。`grep -rn 'throw new\|throws' bindings/jni/example/src/io/mediaengine/example/Run.java` 空，没测 Java listener throws → trampoline 静默 swallow + 不 corrupt JVM 的契约。**方向：** Run.java 加可选 `--throw` flag，listener 在第二次 FRAMES 时 throw RuntimeException；ctest 仍要 exit 0；新 jni_progress_throw_smoke 测试覆盖。Milestone §M7-debt，Rubric §5.5。
- **examples-jni-frame-server-demo** — `bindings/jni/README.md:83` 自 admit "Not exposed yet: me_render_frame"；`grep -rn 'nativeRenderFrame\|me_render_frame' bindings/jni/` 只命中 README。host scrub-row UI 需要 frame-by-frame fetch (不能用 thumbnail，那是 PNG-encode 后)。**方向：** me_jni.cpp 加 `Java_..._nativeRenderFrame` 桥（返回 byte[] RGBA + W/H/stride 通过 Frame record），MediaEngine.java 新 `Frame frame(uri, t)`，新 example/FrameFetch.java 演示。Milestone §M7-debt，Rubric §5.5。
- **debt-bench-thumbnail-budget-tightening** — `bench/bench_thumbnail_png.cpp:37` `kBudgetMs = 50.0`，dev box 实测 ~10 ms（5x headroom）。Loose budget catches 5x regressions 但 miss 2x ones。**方向：** 收紧到 25 ms (2.5x headroom)；如 ctest -j8 contention 下不稳，加 RUN_SERIAL（同 bench_text_paragraph cycle 50 模式）。Milestone §M7-debt，Rubric §5.3。
- **examples-c-frame-server-seek-back** — `examples/08_frame_server_scrub/main.c:124` 只 forward scrub at t=0, 0.5, 1.0, 1.5；real scrub UI 跳跃 + 倒退 + 重复访问。`grep -n 'me_render_frame' examples/08_frame_server_scrub/main.c` 显示 single forward-only loop。**方向：** 新 example 11 或 enhance 08 — 加倒序访问 + 再访问 t=0.5 验 cache hit_count 增长（从 me_cache_stats 比较 before/after counts）。Milestone §M6-debt (cross)，Rubric §5.4。


## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
- **debt-bindings-kn-toolchain-port** — Cycle 83 stood up `kn_cinterop_smoke` ctest gated on gradle <9 because `kotlin("multiplatform") version "2.0.20"` (pinned in `bindings/kotlin-native/example/build.gradle.kts:23`) breaks against Gradle 9 (DefaultArtifactPublicationSet removed). Bumping to Kotlin 2.1+ also breaks `Main.kt:33-39` because the K/N cinterop tool changed how opaque-handle out-params surface (`me_engine_tVar` → `CPointerVar<me_engine>`-ish). On the dev box, gradle is 9.4.1 → ctest currently skips. **方向：** port `Main.kt` to the Kotlin 2.1+ K/N cinterop convention + bump the plugin pin to 2.1.20+; verify `gradle ... runDebugExecutableNative` succeeds; drop the version gate from `bindings/kotlin-native/CMakeLists.txt`. Milestone §M7-debt (cross)，Rubric §5.5。
