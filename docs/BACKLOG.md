# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M2 主线 / 跨 milestone debt）

- **multi-track-video-compose** — 只支持单轨（loader 断言 `tracks.size()==1`）。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。依赖 timeline-asset-map（多 track 共享 asset）+ output-sink-interface（合成后走单一 encode path）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **cross-dissolve-transition** — M2 exit criterion；timeline 里 clip 之间没有 transition 概念。**方向：** timeline schema 加 `track.transitions[]` 数组，每个 transition 描述 `{fromClipId, toClipId, kind: "crossDissolve", duration: rational}`；compose 路径在 overlap 区间做 src/dst alpha 混合。Milestone §M2，Rubric §5.1。
- **ocio-pipeline-enable** — M2 exit criterion `OpenColorIO 集成`。`src/color/pipeline.hpp:74` 的 `make_pipeline()` factory 里 `#if ME_HAS_OCIO` 分支是 dead code——factory 返回 `IdentityPipeline` unconditionally，因为 OCIO FetchContent 之前卡在 nested yaml-cpp CMake policy 底座（见早期 decisions）。**方向：** 重新试 OCIO FetchContent（FFmpeg 之后 libyaml-cpp 可能已经升级了 cmake_minimum；或用 system-dep 走 find_package 路径），成功后把 `ME_WITH_OCIO` 默认 ON，实装 `OcioPipeline` 并在 `make_pipeline()` 返回它，至少支持 bt709 / sRGB / linear 之间的转换。Milestone §M2，Rubric §5.1。
- **debt-examples-cmake-macro-tests** — `examples/CMakeLists.txt` 的 `me_add_example()` 函数（由 `debt-consolidate-example-cmakelists` 引入）支持 `LANG cpp / INTERNAL / EXTRA_LIBS / COPY_RESOURCE` 4 种 option 组合。`grep -rn 'me_add_example' tests/` 返回空——功能靠现有 6 个 example 间接验证；加一个 example 调错参数不会 CI 挂，只会 build 错。**方向：** 简单的 CMake unit test approach 复杂（需要 ctest-subcommand-launch patterns）。折衷：把 `function(me_add_example ...)` 的参数校验收紧（unknown option 报 fatal_error）+ 在 `examples/CMakeLists.txt` 顶部加一段验证 block（configure 期自检函数调用生成正确 target）。Milestone §M2-prep，Rubric §5.2。
- **docs-decisions-dir-readme** — `docs/decisions/` 现在 61 文件（一月落地了 60 个 cycle）。新贡献者想找"ah 上次为啥这么做"的决策很难查——文件名是 `<yyyy-mm-dd>-<slug>`，slug 需要认识 backlog 的 kebab-case 命名才 grep 得到。`docs/decisions/README.md` 只讲"怎么写决策"，不讲"怎么找决策"。**方向：** `docs/decisions/README.md` 末尾加 "Finding a decision" 小节：按模块索引（timeline / orchestrator / color / tests / docs 等各自 grep 模式）+ 按 rubric axis（§5.1 / §5.2 / §5.3）索引。Or 在 `docs/decisions/` 加 `INDEX.md` 分类汇总。Milestone §M2-prep，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **transform-animated-support** — `src/timeline/timeline_loader.cpp` 新 `parse_animated_static_number` 对 `{"keyframes":[...]}` 形式返 `ME_E_UNSUPPORTED` "phase-1: animated (keyframes) form not supported yet"（由 `transform-static-schema-only` cycle 引入的 scope gate）。M3 milestone exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）" 要求把这条拒绝解除。**方向：** 引入 `me::AnimatedNumber` 类型（variant tag `{static \| keyframes}` 或 tagged union），`Transform` 的 8 个字段从 `double` 换成 `AnimatedNumber`；loader 解析两种形式都填 IR；compose / preview 路径在求值时根据 current time 插值。本 bullet 只在 M3 开局动。Milestone §M3，Rubric §5.1。
