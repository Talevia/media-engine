# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M7 主线 / 跨 milestone debt）

- **player-audio-seek-rebuild** — `src/orchestrator/player.cpp:Player::seek` 重置 `audio_dispatched_samples_` 但**不**重建 AudioMixer 或 av_seek_frame 它的 DemuxContexts (`grep -rn 'av_seek_frame\|seek' src/audio/ src/io/demux_context.* 2>/dev/null` 空——audio 路径无 seek primitive)。Phase 5 commit body 显式承认此降级：post-seek 音频内容来自 demux 当前位置（即 t=0 of source），但 chunk_start_t 用新 playhead 重新基线。结果：seek 后音频内容错位 = "press play / pause" 工作但 scrub 音频破。**方向：** 在 `src/io/demux_context.cpp` 加 `seek_to(rational t)` (av_seek_frame + flush decoder)；在 `src/audio/track_feed.cpp` 加 `seek_and_flush(t)` (transitive)；Player::seek 调 build_audio_mixer_for_timeline 重建 mixer 之后 reseek 各 demux。Milestone §M8-debt，Rubric §5.7。
- **debt-bindings-jvm-frame-helper-imageio** — `MediaEngine.Frame.rgba` 是 RGBA8 row-major (cycle 100 doc'd)；JVM hosts 想 wrap 成 `java.awt.image.BufferedImage` 或 javafx WritableImage 都需要 byte-channel re-pack (RGBA→ABGR for TYPE_4BYTE_ABGR)。每个 host 自己写。**方向：** 新 Frame.toBufferedImage() 静态 helper (or `Frames.toBufferedImage(Frame)` util class) 用 java.awt.image API 把 RGBA→BufferedImage TYPE_INT_ARGB；FrameFetch.java 演示。Milestone §M7-debt，Rubric §5.5。
- **examples-c-cache-invalidate-demo** — `include/media_engine/cache.h:40` 暴露 `me_cache_invalidate_asset(eng, content_hash)` 但 `grep -rn 'me_cache_invalidate_asset' examples/` 空——无 demo。host 在 source 文件被外部改动时要 invalidate cache 不知怎么用 (content_hash 怎么算 / 何时调)。**方向：** 新 `examples/12_cache_invalidate/main.c` — render_frame at t=0 (产生 1 entry)，调 me_cache_invalidate_asset，再 render_frame 验 miss_count++。Milestone §M6-debt (cross)，Rubric §5.4。


## P2（未来，当前 milestone 不挤占）

- **player-rate-not-one** — `src/orchestrator/player.cpp:Player::play` 拒绝 rate ≠ 1.0 with ME_E_UNSUPPORTED (`if (rate != 1.0f) return ME_E_UNSUPPORTED;`)。变速预览常用 0.5× / 2× / -1× (倒放)，需要：(a) video frame skip / repeat (rate>1 跳帧、0<rate<1 重复) — 改 producer cursor 推进 + pacer 时间对比；(b) audio tempo via SoundTouch (`src/audio/tempo.hpp:44` 已就位但 Player 路径未接入)；(c) 负 rate 需要反向 demux + 反向 frame queue。**方向：** 先做 0.5..2.0 正向变速 (skip/repeat + audio tempo)；负 rate 单独迭代。Milestone §M8-followup，Rubric §5.5。
- **player-clock-external** — `me_master_clock_kind_t::ME_CLOCK_EXTERNAL` 已在 `include/media_engine/player.h` 占 enum 位但 `me_player_create` 直接返 ME_E_UNSUPPORTED。用例：宿主有非音频外部时钟源 (例如 SMPTE / MIDI clock / 跨进程 IPC clock) 想驱动 Player。**方向：** 加 `me_player_set_external_clock_callback(player, fn, user)`；fn 返 `me_rational_t` 当前时间；Player 在 `PlaybackClock::current` 走 ME_CLOCK_EXTERNAL 分支调 fn。要求 fn lock-free / fast (pacer 每 10 ms 调一次)。Milestone §M8-followup，Rubric §5.5。
- **player-audio-kernel-ize** — Player 当前直接调 `me::audio::AudioMixer::pull_next_mixed_frame` (streaming 路径 b)，不走 graph + scheduler。代价：audio chunk 不进 OutputCache，scrub 重复音频段不命中缓存；多 Player 共享同 timeline 时 audio 重复解码。**方向：** Scheduler 加 streaming Future 支持 (单一 Future yield 多个 chunk)，给 Audio* TaskKindId 注册真 kernel，Player 改用 graph evaluate。重大 scheduler 改造。Milestone §M9+，Rubric §5.4。

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". 跨 repo 任务 — 需要 talevia 那边的工作，本 repo 内能做的就绪后标记完成（`bindings/jni/` 已就绪 at cycle 28 89b7275）。**方向：** 复制 JNI wrapper 到 talevia 目录 + build.gradle.kts + 替换 shell-out FFmpeg 的 passthrough 测试。本 repo cycle 只能准备 JNI wrapper；实际集成在 talevia 仓库。Milestone §M7，Rubric §5.5。
- **ocio-config-env-override** — `grep -rn 'OCIO_CONFIG\|ocio_config_path\|env.*OCIO' src/color` 空；OCIO 目前 hard-wires 内置 config。M8 HDR 要求 host 能传入 custom config (PQ/HLG + ACES)。**方向：** 支持 `OCIO` env var（libOpenColorIO 天然的 LUT 定位环境变量）+ 新 `me_engine_config_t.ocio_config_path` (C ABI append) 覆盖。src/color/ocio_pipeline.cpp 初始化时优先 config path，次 env，末落 default。Milestone §M8，Rubric §5.2。
- **audio-effect-chain-eq-real** — `src/audio/lowpass_audio_effect.hpp:24` 注释 "Future EQ types (high-pass, band-pass, shelving, parametric) fit the same AudioEffect interface"；M4 exit criterion "Audio effect chain" 靠一条 lowpass 通过 ticking，没覆盖真正 parametric EQ。**方向：** 新 `src/audio/peaking_eq_audio_effect.{hpp,cpp}` — biquad 滤波器系数 + 3 参数（freq_hz / gain_db / q）+ AudioMixer 端 integration。Milestone §M4-debt (cross)，Rubric §5.2。
- **me-probe-hdr-metadata** — `include/media_engine/probe.h:56` 注释声称 "HDR10 (10-bit Main10) and SDR 8-bit paths can diverge early" 但 `me_media_info_video_*` 字段只有 bit_depth / color_primaries / color_transfer，没 MaxCLL / master-display / content-light-level。HDR delivery 路径要从 container side-data 抽取。**方向：** 加 `me_media_info_video_max_cll(const me_media_info_t*)` (nits)、`me_media_info_video_master_display_primaries` / `luminance` 等 append-only accessors；probe 端读 `AVFrameSideDataType::AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` + `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL`。Milestone §M8，Rubric §5.2。
- **debt-bindings-kn-toolchain-port** — Cycle 83 stood up `kn_cinterop_smoke` ctest gated on gradle <9 because `kotlin("multiplatform") version "2.0.20"` (pinned in `bindings/kotlin-native/example/build.gradle.kts:23`) breaks against Gradle 9 (DefaultArtifactPublicationSet removed). Bumping to Kotlin 2.1+ also breaks `Main.kt:33-39` because the K/N cinterop tool changed how opaque-handle out-params surface (`me_engine_tVar` → `CPointerVar<me_engine>`-ish). On the dev box, gradle is 9.4.1 → ctest currently skips. **方向：** port `Main.kt` to the Kotlin 2.1+ K/N cinterop convention + bump the plugin pin to 2.1.20+; verify `gradle ... runDebugExecutableNative` succeeds; drop the version gate from `bindings/kotlin-native/CMakeLists.txt`. Milestone §M7-debt (cross)，Rubric §5.5。
