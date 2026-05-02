# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，跨 milestone debt）

- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 records: `me_output_spec_t.video_codec` / `audio_codec` are `const char*`, every codec addition forces a new `is_xxx_spec` helper + `strcmp` branch; `video_bitrate_bps` shared without per-codec partition. With cycle 11's HEVC landing we now have 3 codecs (passthrough, h264, hevc, hevc-sw) and the typed-option-union decision-point has fired per the bullet's threshold. **方向：** typed option union across the C ABI (`me_video_codec_t` enum + per-codec `me_h264_opts_t`, `me_hevc_opts_t`, `me_aac_opts_t` struct + `me_output_spec_t` with tagged-pointer extensions). Major ABI evolution — needs design discussion before implementation; cycle 25 deferred on §3a.10 ABI-break risk. Milestone §M7-debt (cross)，Rubric §5.2。
- **talevia-jvm-wrapper** — M7 exit criterion "在 talevia 内建 platform-impls/video-media-engine-jvm". Cross-repo task — needs talevia-side work; in this repo `bindings/jni/` is ready (cycle 28, commit 89b7275). **方向：** copy JNI wrapper to talevia tree + build.gradle.kts + replace shell-out FFmpeg in passthrough tests. This repo cycle can only prepare the JNI wrapper; integration lives in talevia. Milestone §M7，Rubric §5.5。
- **inverse-tonemap-hable-impl** — Cycle 24 landed `Linear` in `src/compose/inverse_tonemap_kernel.cpp:apply_inverse_tonemap_inplace`; `Hable` algo still returns ME_E_UNSUPPORTED at line 70 (gated by an explicit early-return). Real Hable inverse needs linear-light float buffers: the curve `f(x) = ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F` is invertible only on the [0, white_scale] domain, and byte-domain inverse rounding loses enough precision that the round-trip fails VISION §3.1 byte-identity. **方向：** when a 16-bit (or RgbaF32) working buffer arrives in M11+, replace the Hable UNSUPPORTED branch with the proper linear-light inverse: read SDR byte → linearize via inverse-sRGB-EOTF → numerical inverse of Hable curve via Newton–Raphson on the [0, white_scale] domain → encode to PQ / HLG / linear-light HDR buffer. Until then leave UNSUPPORTED as the deterministic answer for Hable. Milestone §M11-followup，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **examples-14-face-sticker** — M11 exit criterion at `docs/MILESTONES.md:146` requires "每个 effect 有像素级回归测试 + 至少一个 example（候选：`12_face_sticker`、`13_body_alpha_key`）". `ls examples/` shows 10_player_pause_seek / 11_error_diagnosis / 12_cache_invalidate occupy slots 10-12; 14+ are free. **方向：** new `examples/14_face_sticker/` with `main.cpp` + `sample.timeline.json` + a tiny PNG sticker fixture + a JSON landmark fixture. Demonstrates timeline → render → output MP4 with the sticker overlaid. Mirrors examples/01_passthrough's shape: link media_engine, parse timeline JSON, call render_start, write output. Milestone §M11，Rubric §5.1。
- **examples-15-body-alpha-key** — sibling of `examples-14-face-sticker` for body_alpha_key. **方向：** `examples/15_body_alpha_key/` with timeline + mask fixture + tiny PNG. Demonstrates green-screen-without-greenscreen via the body_alpha_key effect. Same shape as #12. Milestone §M11，Rubric §5.1。
- **encode-hevc-main10-kvazaar-source-build** — `src/io/kvazaar_hevc_encoder.cpp:88` calls `kvz_api_get(8)` exclusively; the homebrew Kvazaar bottle's `kvazaar.h` defaults to `KVZ_BIT_DEPTH 8` and the prebuilt library doesn't expose a 10-bit `kvz_pixel` ABI. `cfg->input_bitdepth = 10` is accepted by `encoder_open` but the `picture_alloc` buffer geometry stays 8-bit. M10 exit criterion 3 (docs/MILESTONES.md:120) was satisfied by the M10 SW HEVC fallback shipping Main 8-bit only; HDR Main 10 stays on the VideoToolbox HW path. This bullet is the future-improvement to bring SW HEVC up to Main 10 for cross-host HDR. **方向：** replace `pkg_check_modules(kvazaar)` with `FetchContent_Declare(kvazaar URL ...)` + `cmake -DKVZ_BIT_DEPTH=10` so the engine links its own 10-bit-built libkvazaar.a; rewrite the encoder TU's plane-copy loops to pass uint16_t buffers. Add a fixture YUV420P 10-bit gray frame and a round-trip test that decodes the produced HEVC Main 10 and verifies pixel domain matches within ε. Note: Kvazaar's build system is autotools (no native CMake); FetchContent will need ExternalProject_Add wrapping. Milestone §M10-followup，Rubric §5.2。
