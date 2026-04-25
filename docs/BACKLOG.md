# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）

- **examples-jni-thumbnail-batch-demo** — `grep -c 'thumbnail(' bindings/jni/example/src/io/mediaengine/example/Thumbnail.java` 返回 2（一次声明 + 一次调用）。real scrub-row UI 一次拉 N=10-50 个 thumbnails (一格一缩略图)；当前 demo 不展示 batch loop pattern。**方向：** 新 ThumbnailBatch.java demo — 跑 5 个 t={0, 0.4, 0.8, 1.2, 1.6}s 的 thumbnails，写到 `<out-dir>/thumb_<i>.png`；展示 cache hit growth (cycle 94 example_08 同样模式但用 me_render_frame)。Milestone §M7-debt，Rubric §5.5。

## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **debt-bindings-jvm-engine-config-cache-dir** — `grep -n 'cache_dir' bindings/jni/src/io/mediaengine/MediaEngine.java bindings/jni/me_jni.cpp` 空：MediaEngine ctor 调 `me_engine_create(NULL, &eng)` (cycle 28，固定 NULL config)。host 想 enable disk cache (cycle 39 落地的 me_engine_config_t.cache_dir) 在 JVM 端没桥。**方向：** MediaEngine.java 增 `MediaEngine(String cacheDir)` overload；me_jni.cpp 加 nativeCreateWithConfig(String cacheDir) 桥；老 nativeCreate 保留 backward-compat。Milestone §M7-debt，Rubric §5.5。
- **debt-jni-thumbnail-arg-validation** — `bindings/jni/me_jni.cpp:Java_..._nativeThumbnail` 把 jint maxWidth/maxHeight 直接传 me_thumbnail_png；负值 / 极大值行为不文档化。`grep -n 'thumbnail.*-1\|thumbnail.*Integer.MAX_VALUE' bindings/jni/example/` 空——无 negative-arg 测试。**方向：** Java side 在 thumbnail() 抛 IllegalArgumentException if maxWidth/Height < 0；javadoc 声明 0 = native dim, 正数 = max bounding box；新 jni_thumbnail_args ctest 验拒 negative。Milestone §M7-debt，Rubric §5.5。
- **debt-bindings-jvm-frame-helper-imageio** — `MediaEngine.Frame.rgba` 是 RGBA8 row-major (cycle 100 doc'd)；JVM hosts 想 wrap 成 `java.awt.image.BufferedImage` 或 javafx WritableImage 都需要 byte-channel re-pack (RGBA→ABGR for TYPE_4BYTE_ABGR)。每个 host 自己写。**方向：** 新 Frame.toBufferedImage() 静态 helper (or `Frames.toBufferedImage(Frame)` util class) 用 java.awt.image API 把 RGBA→BufferedImage TYPE_INT_ARGB；FrameFetch.java 演示。Milestone §M7-debt，Rubric §5.5。
- **examples-c-cache-invalidate-demo** — `include/media_engine/cache.h:40` 暴露 `me_cache_invalidate_asset(eng, content_hash)` 但 `grep -rn 'me_cache_invalidate_asset' examples/` 空——无 demo。host 在 source 文件被外部改动时要 invalidate cache 不知怎么用 (content_hash 怎么算 / 何时调)。**方向：** 新 `examples/12_cache_invalidate/main.c` — render_frame at t=0 (产生 1 entry)，调 me_cache_invalidate_asset，再 render_frame 验 miss_count++。Milestone §M6-debt (cross)，Rubric §5.4。


## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
- **debt-bindings-kn-toolchain-port** — Cycle 83 stood up `kn_cinterop_smoke` ctest gated on gradle <9 because `kotlin("multiplatform") version "2.0.20"` (pinned in `bindings/kotlin-native/example/build.gradle.kts:23`) breaks against Gradle 9 (DefaultArtifactPublicationSet removed). Bumping to Kotlin 2.1+ also breaks `Main.kt:33-39` because the K/N cinterop tool changed how opaque-handle out-params surface (`me_engine_tVar` → `CPointerVar<me_engine>`-ish). On the dev box, gradle is 9.4.1 → ctest currently skips. **方向：** port `Main.kt` to the Kotlin 2.1+ K/N cinterop convention + bump the plugin pin to 2.1.20+; verify `gradle ... runDebugExecutableNative` succeeds; drop the version gate from `bindings/kotlin-native/CMakeLists.txt`. Milestone §M7-debt (cross)，Rubric §5.5。
