# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **debt-jni-error-status-mapping** — `bindings/jni/me_jni.cpp:104` `nativeLoadTimeline` 返回 0 on failure；`bindings/jni/src/io/mediaengine/MediaEngine.java:39-43` 把 0 翻译成 RuntimeException，但 Java 端无法区分 ME_E_PARSE / ME_E_INVALID_ARG / ME_E_IO 等具体 status。host 想做 graceful retry 拿不到 status code。**方向：** 新 `nativeLastStatus(long engine) → int` JNI 桥（exposes `me_engine_last_status` if it exists, else thread-local set in nativeLoadTimeline before返回 0）；MediaEngine.java exception 类型按 status 分流。Milestone §M7-debt，Rubric §5.5。
- **schema-tests-text-clip-color-hex-cases** — `grep -rn 'AnimatedColor::from_hex' src/timeline/animated_color.cpp` 走 4 路 (RRGGBB / RRGGBBAA / #RGB / #RGBA)；`grep -rn 'from_hex\|color.*#' tests/test_animated_color.cpp` 命中现有 cases，但缺 invalid-hex（"#GG", "#1234", "rgb(...)" 之类）reject 路径覆盖。一次 from_hex parser regression 把无效字符串 silently coerce 成 0,0,0,0 现在 ctest 不抓。**方向：** 新 SUBCASE in test_animated_color.cpp — invalid hex strings → from_hex 返回 default 而不 throw / not crash；明示文档化 invalid-input 行为。Milestone §M5-debt (cross)，Rubric §5.3。
- **debt-jni-version-rebuild-discipline** — Cycle 77 incident: `cmake --build build --target test_bench_harness` 不重 link libmedia_engine_jni.dylib，jni_version_assert_smoke 与 ME_GIT_SHA mismatch。CMake 依赖图正确（test_bench_harness depends on media_engine 经 alias）但 incremental single-target 不传到 JNI lib。**方向：** 在 bindings/jni/CMakeLists.txt 加 `add_dependencies(jni_version_assert_smoke media_engine_jni)`（语法上是 set_tests_properties FIXTURES_REQUIRED），或在 README 写 "ctest 前先 cmake --build build (no target)"；选其中一即可。Milestone §M7-debt，Rubric §5.3。


## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
- **debt-bindings-kn-toolchain-port** — Cycle 83 stood up `kn_cinterop_smoke` ctest gated on gradle <9 because `kotlin("multiplatform") version "2.0.20"` (pinned in `bindings/kotlin-native/example/build.gradle.kts:23`) breaks against Gradle 9 (DefaultArtifactPublicationSet removed). Bumping to Kotlin 2.1+ also breaks `Main.kt:33-39` because the K/N cinterop tool changed how opaque-handle out-params surface (`me_engine_tVar` → `CPointerVar<me_engine>`-ish). On the dev box, gradle is 9.4.1 → ctest currently skips. **方向：** port `Main.kt` to the Kotlin 2.1+ K/N cinterop convention + bump the plugin pin to 2.1.20+; verify `gradle ... runDebugExecutableNative` succeeds; drop the version gate from `bindings/kotlin-native/CMakeLists.txt`. Milestone §M7-debt (cross)，Rubric §5.5。
