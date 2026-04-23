# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M1 收尾或 M2 起步）

- **me-probe-more-fields** — `me_media_info_*` 返回 container / codec / duration / W×H / 帧率 / sample_rate / channels，已经满足 M1 exit。但 M2 compose / OCIO 需要更多：rotation（iOS 竖屏视频常见）、color_range（limited vs full）、color_primaries / transfer / matrix（避免色彩转换错）、bit_depth。**方向：** 扩 `me_media_info_*` accessor（ABI 末尾 append），loader 端已经能从 `AVCodecParameters` 拿到，只需暴露。Milestone §M2-prep / Rubric §5.1。
- **debt-output-sink-smoke-test** — `make_output_sink` 现在只有 Exporter 一个 caller；`PassthroughSink` / `H264AacSink` 各自的 `process()` 是被集成在 `01_passthrough` / `05_reencode` 里间接测的。工厂返回 nullptr 的 error 路径（unsupported spec、空 clip ranges、reencode + multi-clip）没有直接断言。**方向：** 新 `tests/test_output_sink.cpp`：factory 覆盖支持的两种 spec + 4 类拒绝（null path、empty ranges、h264+passthrough 混搭、h264/aac + 2 clips）。不跑 process，只断言 factory 返回值 + err 串。Milestone §M1-debt，Rubric §5.2。
- **debt-consolidate-example-cmakelists** — `examples/01_passthrough/` ~ `06_thumbnail/` 每个目录一个 2-行 CMakeLists.txt，内容完全一样 (`add_executable + target_link_libraries`)，加 01_passthrough 还额外复制 sample.timeline.json。未来每加一个 example 就要再复制一份。**方向：** `examples/CMakeLists.txt` 顶部加 `function(me_add_example name)` 封装；各子目录 CMakeLists 降到 `me_add_example(01_passthrough)`。01_passthrough 的 post-build copy 作为函数的可选参数。Milestone §M1-debt，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **ocio-integration** — 暂无色彩管理。**方向：** OpenColorIO FetchContent，assets 的 colorSpace → 工作空间转换 → 输出空间。依赖 asset-colorspace-field 先落。Milestone §M2，Rubric §5.1。
- **multi-track-video-compose** — 只支持单轨。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。依赖 timeline-asset-map（多 track 共享 asset）+ output-sink-interface（合成后走单一 encode path）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **debt-schema-version-migration-hook** — schema v1 rejection 只认 `== 1`，没有 v2 迁移预演。**方向：** loader 里留 `migrate(v_from, v_to)` 接口，即使只支持 v1 也显式走一遍 migration path，未来 v2 接入零改动。Milestone §M2，Rubric §5.1。
- **reencode-multi-clip** — reencode path 仍只支持 single clip；passthrough 已支持 concat。**方向：** 每 clip 重置 encoder GOP bookkeeping + AAC priming 在边界处理；同 codec 可复用 encoder ctx（可参考 codec-pool-impl），不同 codec 必须新开 encoder。Milestone §M1-addendum，Rubric §5.1。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转（PAIN_POINTS 2026-04-23）。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口；现在记录不做。Milestone §M6-prep，Rubric §5.2。
- **codec-pool-real-pooling** — CodecPool 目前只跟踪 live_count，不做真 reuse（见 codec-pool-impl 决策：FFmpeg encoder 跨 stream 不安全）。真正能 pool 的是 decoder（`avcodec_flush_buffers` 可用来复位状态）。**方向：** 等 `reencode-multi-clip` 或 M4 多段音频需要反复开同 codec 时，加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` 的 pool 路径。现在是 debt 占位，等 consumer 再动。Milestone §M4-prep，Rubric §5.3。
- **docs-architecture-graph-two-exec-models** — `ARCHITECTURE_GRAPH.md` 只描述 "graph per-frame + scheduler" 一种执行模型，但 M1 reencode path 实际走 orchestrator streaming（`reencode_pipeline` 自持 decode→encode→mux，不走 graph per-frame）。M4–M6 frame server 上线后两种模型并存，文档不写会让读者误以为 graph 是唯一路径。**方向：** 在 `ARCHITECTURE_GRAPH.md` 顶部加一节"两种执行模型"，明确 (a) graph per-frame（preview / thumbnail / cache）、(b) orchestrator streaming（export / re-encode），给出何时选哪种的判别标准（stateful streaming vs stateless per-frame）。Milestone §M2-prep，Rubric §5.2。
- **debt-timeline-loader-engine-seed-pattern** — `me_timeline_load_json`（`src/api/timeline.cpp`）在 loader 返回后循环灌 `engine->asset_hashes`。loader 自己 engine-agnostic（`me::timeline::load_json` 只吃 JSON），结果 extern "C" 入口同时扮演"薄胶水"和"业务副作用触发点"两份角色。现在只 4 行，未来每加一种"load 时要 seed 的 engine 资源"（color pipeline、effect LUT 预热…）都要在这里再加一圈，pattern 不扩展。**方向：** 评估两条路：(a) `me::timeline::load_json` 签名吃 `Engine*`，loader 自带 seed（牺牲 engine-agnostic）；(b) Timeline 暴露 `apply_to_engine(Engine&)` hook，C API 调一次（保持 loader 纯净，引入新概念）。等 M2 第二种 seed 资源出现再定。Milestone §M2-debt，Rubric §5.2。
- **debt-stub-count-source-unify** — `tools/scan-debt.sh` §2 跑 `grep return ME_E_UNSUPPORTED` 算粗粒度总数（当前 11）；`tools/check_stubs.sh` 按 `STUB:` 标记算精细条目数（当前 3）。8 条 drift 说明有 stub 没打标记，但两份脚本各跑各的没人报警——新 runtime-reject path（scan-debt 涨、check-stubs 不动）或新 stub 忘了标记（check-stubs 漏、scan-debt 涨）都滑过监控。**方向：** `STUB:` 标记成为唯一事实源；`scan-debt.sh` §2 改成一致性检查"raw count − marked stubs 应为 0，否则列出未标记行号"，把 drift 变成显式告警。Milestone §M1-debt，Rubric §5.2。
