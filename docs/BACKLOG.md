# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **debt-split-compose-sink-cpp** — `wc -l src/orchestrator/compose_sink.cpp` = 399（scan-debt §1a 阈值 400 一行之差）。下一个 compose-sink feature commit 几乎肯定推过线触发 P0。`grep -n '^[a-zA-Z].*::' src/orchestrator/compose_sink.cpp` 显示 `is_gpu_compose_usable` (line 304) 和 `make_compose_sink` factory (line 318) 是天然分割线。**方向：** 抽 `src/orchestrator/compose_sink_gpu_dispatch.{hpp,cpp}` 装 GPU 检测 + dispatch shim；compose_sink.cpp 留 sink-trait + factory，预计降到 ~250 行。Milestone §M7-debt (cross)，Rubric §5.3。
- **debt-split-compose-decode-loop-cpp** — `wc -l src/orchestrator/compose_decode_loop.cpp` = 387，与 bullet 4 同样"一行之差"。文件混着 (a) decode-loop 顶层驱动 + (b) Skia text-clip 分支（cycle 29/49） + (c) ME_HAS_LIBASS subtitle 分支（cycle 30/41）。**方向：** 抽 text/subtitle render 分支到 `compose_decode_renderers.{hpp,cpp}`（或者按 ME_HAS_SKIA / ME_HAS_LIBASS 分两份文件），主 loop 收回到 ~250 行。Milestone §M7-debt (cross)，Rubric §5.3。
- **examples-jni-jvm-runner** — `bindings/jni/src/io/mediaengine/MediaEngine.java` 有 wrapper 但 `find bindings/jni -name 'Run*.java' -o -name '*Example*'` 空；M7 cycle 28 89b7275 commit body 承诺 "host-ready"，但本 repo 无端到端 JVM 调用 demo，talevia 集成者要自己拼。**方向：** 加 `bindings/jni/example/Run.java` (~50 行) — System.loadLibrary + 内嵌 passthrough timeline JSON + render_start + 监听 progress + 打印 version；CMake `add_test` JAVA_HOME 存在时 javac+java 全跑通。Milestone §M7-debt，Rubric §5.5。
- **debt-bindings-jni-version-string-validate** — `bindings/jni/me_jni.cpp:215` `Java_io_mediaengine_MediaEngine_nativeVersion` 包了 `me_engine_version()` 返回 jstring，但没测 JNI 端返回的字符串等于 C 端 `me_engine_version_string()`。一次 engine version 重构（如 add patch suffix）可静默漂移。**方向：** ctest 通过 `find_package(Java)` JNI 调用 nativeVersion，与 `me_engine_version_string()` 字符串相等比较；无 Java 时 silent-skip。Milestone §M7-debt，Rubric §5.5。
- **examples-jni-thumbnail-jvm-demo** — `examples/` 从 06_thumbnail 直接跳到 07_compose_multitrack；`grep -rn 'me_thumbnail\|nativeThumbnail' bindings/jni/` 空——MediaEngine.java 只 wrap 了 render_start，没有 thumbnail。host scrub-row UI 是 thumbnail 的主消费者，少了这条 hosts 不知怎么从 JVM 取 thumbnail。**方向：** 在 MediaEngine.java 加 `nativeThumbnail` + `me_jni.cpp` 加 `Java_io_mediaengine_MediaEngine_nativeThumbnail` 桥；新 `examples/10_jni_thumbnail/Run.java` 演示从 mp4 取 0.5s 的 PNG bytes。Milestone §M7-debt，Rubric §5.5。
- **debt-test-bench-harness-coverage** — `bench/bench_harness.hpp` (cycle 68) 只有两个 bench 间接使用，`grep -rn 'measure_avg_sec\|me::bench::' tests/` 空。template 的 "iters <= warmup → 返回 0.0" 边界契约没显式覆盖；将来回归（如把 0.0 改成 NaN 或 -1）只能等 bench 跑挂才发现。**方向：** 新 `tests/test_bench_harness.cpp` doctest TEST_CASE: (a) iters>warmup happy path，验 work 调用次数 + 返回 ≈ sleep 时长，(b) iters<=warmup 返回 0.0，(c) work 是 lambda capturing 计数 verify call count = iters。Milestone §M7-debt (cross)，Rubric §5.3。


## P2（未来，当前 milestone 不挤占）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
