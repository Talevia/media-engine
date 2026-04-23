# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M1 收尾或 M2 起步）

- **test-thumbnail-coverage** — `me_thumbnail_png` 已 impl 但 `tests/` 无覆盖；未来 color-space 正确性改动没有 tripwire。**方向：** `tests/test_thumbnail.cpp`：用 determinism fixture，`me_thumbnail_png(engine, asset_id, {0,10}, 320, 240, &buf, &size)` → 断言 PNG magic `89 50 4E 47` + IHDR width/height = 320/240。Milestone §M1-debt，Rubric §5.2。
- **debt-stub-count-source-unify** — `tools/scan-debt.sh` §2 按 grep 数 `ME_E_UNSUPPORTED` = 11；`tools/check_stubs.sh` 按 `STUB:` 标记 = 3。8 条 drift 没人报警；新 runtime-reject 或新 stub 忘打标记都滑过监控。**方向：** 把 `STUB:` 标记改成唯一事实源；`scan-debt.sh` §2 变一致性检查"raw − marked 应为 0 否则列出未标记行号"。Milestone §M1-debt，Rubric §5.2。
- **debt-split-reencode-pipeline-audio-fifo** — `src/orchestrator/reencode_pipeline.cpp` 在 `debt-split-reencode-pipeline` 后曾降到 300+ 行，现在又涨回 437 行（scan-debt 2026-04-23 snapshot）——主要是 `drain_audio_fifo` lambda + AAC priming 逻辑嵌在 `reencode_mux` 里。加 `reencode-multi-clip` 时会再涨。**方向：** 把 audio FIFO 循环从 lambda 提成独立 `me::orchestrator::drain_audio_fifo()` TU 级函数（签名吃 AVAudioFifo*, aenc, ofmt, out_aidx, &next_pts, &terminal, err）放到 `reencode_audio.cpp/hpp`。Milestone §M1-debt，Rubric §5.2。
- **ocio-integration-skeleton** — OpenColorIO 集成是 M2 硬条件但现在仓库一行 OCIO 代码都没有。一次性落大块会冲击 PR；拆成"先 FetchContent 进 CMake + 搭 `me::color` 命名空间 + identity pipeline" 这一步骨架就能验证依赖链接 + `ARCHITECTURE.md` 白名单加条 OCIO 条目。**方向：** `FetchContent_Declare(OCIO ...)` + `find_package(OCIO REQUIRED)` + `src/color/pipeline.hpp` 定义 `me::color::IdentityPipeline`（什么都不做，just pass-through），`ARCHITECTURE.md` 的 License table 加 OCIO (BSD)。不接 asset-colorspace-field，下一轮再接。Milestone §M2-prep，Rubric §5.1。
- **docs-architecture-graph-two-exec-models** — `ARCHITECTURE_GRAPH.md` 只写了 "graph per-frame + scheduler" 一种执行模型，但 M1 reencode path 走 orchestrator streaming（`reencode_pipeline` 自持 decode→encode→mux）不走 graph per-frame。两种模型并存不写文档会让读者误以为 graph 是唯一路径。**方向：** `ARCHITECTURE_GRAPH.md` 顶部加"两种执行模型"小节：(a) graph per-frame（preview / thumbnail / cache），(b) orchestrator streaming（export / re-encode），给判别标准（stateful streaming vs stateless per-frame）。Milestone §M2-prep，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **debt-timeline-loader-engine-seed-pattern** — `me_timeline_load_json`（`src/api/timeline.cpp`）在 loader 返回后循环灌 `engine->asset_hashes`。loader 自己 engine-agnostic，extern "C" 入口同时扮演"薄胶水"和"业务副作用触发点"两份角色；pattern 不扩展。**方向：** 等 M2 第二种 seed 资源（color pipeline / effect LUT 预热）出现再定两条路：(a) `me::timeline::load_json` 吃 `Engine*`，牺牲 engine-agnostic；(b) Timeline 暴露 `apply_to_engine(Engine&)` hook，保持 loader 纯净。Milestone §M2-debt，Rubric §5.2。
- **multi-track-asset-reuse-test** — `timeline-asset-map` 决定已把 Asset 提成 first-class IR 并承诺"多 clip 共享 asset 时只开一份 DemuxContext"，但没有 test 直接断言这个行为。未来重构 DemuxContext ownership 模型时会无声破坏。**方向：** `tests/test_asset_reuse.cpp`：两 clip 引用同一 assetId，跑 Exporter 到 passthrough，断言 `CodecPool::live_count()` 峰值 ≤ 1（或 FramePool 同理）。Milestone §M1-debt，Rubric §5.2。
- **multi-track-video-compose** — 只支持单轨。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。依赖 timeline-asset-map（多 track 共享 asset）+ output-sink-interface（合成后走单一 encode path）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **codec-pool-real-pooling** — CodecPool 目前只跟踪 live_count，不做真 reuse（见 codec-pool-impl 决策：FFmpeg encoder 跨 stream 不安全）。真正能 pool 的是 decoder（`avcodec_flush_buffers` 可复位状态）。**方向：** 等 `reencode-multi-clip` 或 M4 多段音频需要反复开同 codec 时，加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` 的 pool 路径。现在是 debt 占位，等 consumer 再动。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口；现在记录不做。Milestone §M6-prep，Rubric §5.2。
