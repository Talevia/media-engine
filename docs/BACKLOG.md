# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）

- **me-asset-color-space-thread-to-encoder** — `ocio-pipeline-wire-first-consumer` cycle 把 `me::color::make_pipeline()` 接进 reencode 路径，但 `src/orchestrator/reencode_segment.cpp:210` 写死 `const me::ColorSpace dummy{};` 传给 `pipeline->apply`。真正的 asset-level `me::Asset::color_space`（`me::timeline::Asset::color_space` std::optional<ColorSpace>，loader 在 `asset-colorspace-field` cycle 填充）**没有**被 thread 到 SharedEncState 或 process_segment。ME_WITH_OCIO 切 ON 那天真 OcioPipeline 会拿到 default-constructed ColorSpace → 识别成 unspecified → 无 transform → 效果等于未接入。**方向：** `SharedEncState` 加 `me::ColorSpace source_color_space` + `me::ColorSpace target_color_space`；`reencode_mux` 在 `ReencodeSegment` / `ReencodeOptions` 通路上接收 timeline working-space + segment[0] asset 的 color_space；`push_video_frame` 传真值给 `apply()`。Milestone §M2-prep，Rubric §5.1。
- **debt-docs-api-md-stale-audit** — `docs/API.md` 163 行，最近 8 周有 me_probe 扩 6 accessor、reencode-multi-clip 落地、cache_stats 实装、`me_buffer_free` 加入等 API 变化。没人 audit 过 docs/API.md 还是否匹配当前 header 接口。**方向：** diff `include/media_engine/*.h` 的公开声明与 `docs/API.md` 描述；缺的补上、stale 的改；不出现已 deleted / renamed 的 symbol。写成 `docs(api)` 或 `refactor(docs)` commit。Milestone §M1-debt，Rubric §5.2。

## P1（强烈建议，M1 收尾或 M2 起步）

- **debt-test-reencode-cancel-mid-render** — `me_render_cancel` + `Job::cancel` atomic flag 是公共 API（`grep -rn 'cancel' src/api/render.cpp` 确认 exposed），但 `grep -rn 'me_render_cancel\|cancel' tests/` 返回空——零 doctest 覆盖 cooperative cancellation。未来 tweak cancel-check 频率 / race condition / 双 cancel call 的行为都会 silently regress。**方向：** `tests/test_render_cancel.cpp`（或塞进 test_determinism）：start render → 主线程 200ms 后 cancel → `me_render_wait` 返回 `ME_E_CANCELLED` + 产物文件不存在 / 残缺状态明确。Milestone §M1-debt，Rubric §5.2。
- **debt-test-progress-callback-sequencing** — `me_progress_event_t` 暴露 STARTED / FRAMES / COMPLETED / FAILED 4 种 kind（`include/media_engine/render.h`），host UI 靠 STARTED 初始化 progress bar、COMPLETED 收尾。`grep -rn 'ME_PROGRESS\|progress_cb\|on_progress' tests/` 返回空——**零**断言事件顺序（STARTED 恰好一次 + FRAMES N 次 + COMPLETED 恰好一次 + 无 FAILED；反之：FAILED 一次 + 无 COMPLETED）。重构 Exporter worker thread 时极易 silently 发错顺序。**方向：** `tests/test_render_progress.cpp`：capture callback events into `std::vector`，断言正常路径的序列正好是 `[STARTED, FRAMES*, COMPLETED]`，取消路径是 `[STARTED, FRAMES*, FAILED]`。Milestone §M1-debt，Rubric §5.2。
- **debt-test-render-job-lifecycle** — `me_render_job_destroy` 当 job 未 wait 时行为未测：`grep -rn 'me_render_job_destroy' tests/` 返回空。现有 test 都走 wait→destroy；没断言 destroy-without-wait（host 取消后 forget 的场景）或 destroy-twice（host bug 的防御）。**方向：** `tests/test_render_job.cpp`：(a) start → destroy-without-wait → no crash / leak，(b) start → wait → destroy → 再 destroy null ptr → no crash。Milestone §M1-debt，Rubric §5.2。
- **me-render-output-format-infer** — `me_output_spec_t.container` 为 NULL 时，`MuxContext::open` 传 nullptr 给 `avformat_alloc_output_context2` 让 libav 按 out_path 扩展名推断。没问题，但 host 传 "foo.xyz" 的未知扩展时行为 unclear——silently 失败 or 报清晰错？**方向：** `tests/test_output_spec.cpp` 加一 case：null container + 未知扩展名 → `me_render_start` 返回 `ME_E_UNSUPPORTED` 或类似，err 含 "container format not recognised"；known 扩展名（.mp4 / .mov）正常路径。Milestone §M1-debt，Rubric §5.1。
- **transform-static-schema-only** — M2 exit criterion `Transform (静态)` 还没开始；先把 JSON schema 落地避免真写 compose 时要同步改 loader 正则。`grep -rn 'transform' docs/TIMELINE_SCHEMA.md src/timeline/timeline_loader.cpp` 在 loader 里只命中 `me_rational_t time_start` 之类，没 Transform concept。**方向：** `docs/TIMELINE_SCHEMA.md` 加 `clip.transform?: { translate?: {x,y}, scale?: {x,y}, rotation?: rational_degrees, opacity?: 0..1 }` 段，loader 解析该字段填充 `me::Clip::transform` std::optional<Transform>——**不接** compose（那是 multi-track-video-compose 之后）。Milestone §M2-prep，Rubric §5.1。
- **debt-examples-cmake-macro-tests** — `examples/CMakeLists.txt` 的 `me_add_example()` 函数（由 `debt-consolidate-example-cmakelists` 引入）支持 `LANG cpp / INTERNAL / EXTRA_LIBS / COPY_RESOURCE` 4 种 option 组合。`grep -rn 'me_add_example' tests/` 返回空——功能靠现有 6 个 example 间接验证；加一个 example 调错参数不会 CI 挂，只会 build 错。**方向：** 简单的 CMake unit test approach 复杂（需要 ctest-subcommand-launch patterns）。折衷：把 `function(me_add_example ...)` 的参数校验收紧（unknown option 报 fatal_error）+ 在 `examples/CMakeLists.txt` 顶部加一段验证 block（configure 期自检函数调用生成正确 target）。Milestone §M2-prep，Rubric §5.2。
- **docs-decisions-dir-readme** — `docs/decisions/` 现在 61 文件（一月落地了 60 个 cycle）。新贡献者想找"ah 上次为啥这么做"的决策很难查——文件名是 `<yyyy-mm-dd>-<slug>`，slug 需要认识 backlog 的 kebab-case 命名才 grep 得到。`docs/decisions/README.md` 只讲"怎么写决策"，不讲"怎么找决策"。**方向：** `docs/decisions/README.md` 末尾加 "Finding a decision" 小节：按模块索引（timeline / orchestrator / color / tests / docs 等各自 grep 模式）+ 按 rubric axis（§5.1 / §5.2 / §5.3）索引。Or 在 `docs/decisions/` 加 `INDEX.md` 分类汇总。Milestone §M2-prep，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **multi-track-video-compose** — 只支持单轨（loader 断言 `tracks.size()==1`）。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。依赖 timeline-asset-map（多 track 共享 asset）+ output-sink-interface（合成后走单一 encode path）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **cross-dissolve-transition** — M2 exit criterion；timeline 里 clip 之间没有 transition 概念。**方向：** timeline schema 加 `track.transitions[]` 数组，每个 transition 描述 `{fromClipId, toClipId, kind: "crossDissolve", duration: rational}`；compose 路径在 overlap 区间做 src/dst alpha 混合。Milestone §M2，Rubric §5.1。
- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 M4 多段音频或 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
