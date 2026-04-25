# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **debt-jni-cmake-classfile-list-foreach** — `bindings/jni/CMakeLists.txt:84-118` 维护 8 .java 文件的 _src + _classfile 双 list，每加 demo 要 +4 行模板。`grep -c '_me_jni_.*_src' bindings/jni/CMakeLists.txt` 显示 8 平行 src vars + 8 平行 classfile vars + 8 entries 在 add_custom_command DEPENDS。**方向：** foreach over a single list of `<class-stem>` strings，循环 derive `_src` + `_classfile` paths；保留同名 vars 给后向兼容（or remove altogether 因为只在本文件用）。Milestone §M7-debt，Rubric §5.3。
- **debt-jni-frame-record-doc-channel-order** — `bindings/jni/src/io/mediaengine/MediaEngine.java:Frame` doc 只说 "byte[] rgba" + "Java-managed copy"；不说 channel order (RGBA vs BGRA vs ABGR)、stride (width*4 vs padded)、color space (sRGB vs linear)。Host integrator 拿到 byte[] 不知怎么读。**方向：** Frame record javadoc + 行内 comment 标 "RGBA8 row-major, stride = width*4, sRGB primaries"；同步 me_render_frame 的 C 端 doc-comment。Milestone §M7-debt，Rubric §5.4。
- **examples-c-render-error-diagnosis** — `grep -A 5 'me_render_wait\|rc != ME_OK\|me_engine_last_error' examples/01_passthrough/main.c examples/05_reencode/main.c` 显示现有 examples 都在 rc != ME_OK 时 fputs error + return 1。没有"分类失败 status 决定 retry / bail"的 demo（host scrub UI 的真实 pattern）。**方向：** 新 `examples/11_error_diagnosis/main.c` — 故意触发 3 类失败（malformed JSON → ME_E_PARSE，nonexistent file://path → ME_E_IO，未知 codec → ME_E_UNSUPPORTED），打印 status + last_error 演示 switch-on-status retry logic。Milestone §M7-debt，Rubric §5.4。
- **debt-bench-vfr-av-sync-output-doc** — `bench_vfr_av_sync` 在 ctest 跑（`grep -A 1 'add_test.*bench_vfr_av_sync' bench/CMakeLists.txt`）；但 source 内的 budget comment（`grep '1 ms / hour\|drift'` bench/bench_vfr_av_sync.cpp）没有最近测量 vs budget 的对照数据。一次 av-sync regression（off-by-one PTS rescale）只在 budget 被超时才触发；建议把当前实测 drift 数据加进 source comment 作 baseline。**方向：** 跑 bench 3 次取 max drift；写进 bench_vfr_av_sync.cpp 注释 + 收紧 budget 到 2x headroom（同 cycle 93 模式）。Milestone §M3-debt (cross)，Rubric §5.3。


## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
- **debt-bindings-kn-toolchain-port** — Cycle 83 stood up `kn_cinterop_smoke` ctest gated on gradle <9 because `kotlin("multiplatform") version "2.0.20"` (pinned in `bindings/kotlin-native/example/build.gradle.kts:23`) breaks against Gradle 9 (DefaultArtifactPublicationSet removed). Bumping to Kotlin 2.1+ also breaks `Main.kt:33-39` because the K/N cinterop tool changed how opaque-handle out-params surface (`me_engine_tVar` → `CPointerVar<me_engine>`-ish). On the dev box, gradle is 9.4.1 → ctest currently skips. **方向：** port `Main.kt` to the Kotlin 2.1+ K/N cinterop convention + bump the plugin pin to 2.1.20+; verify `gradle ... runDebugExecutableNative` succeeds; drop the version gate from `bindings/kotlin-native/CMakeLists.txt`. Milestone §M7-debt (cross)，Rubric §5.5。
