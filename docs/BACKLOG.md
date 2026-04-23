# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M1 收尾或 M2 起步）

- **me-probe-more-fields** — `me_media_info_*` 返回 container / codec / duration / W×H / 帧率 / sample_rate / channels，已经满足 M1 exit。但 M2 compose / OCIO 需要更多：rotation（iOS 竖屏视频常见）、color_range（limited vs full）、color_primaries / transfer / matrix（避免色彩转换错）、bit_depth。**方向：** 扩 `me_media_info_*` accessor（ABI 末尾 append），loader 端已经能从 `AVCodecParameters` 拿到，只需暴露。Milestone §M2-prep / Rubric §5.1。
- **debt-fixture-gen-libav** — `test_determinism` 生成 fixture 依赖系统 ffmpeg CLI（PAIN_POINTS 2026-04-23）；没 ffmpeg CLI 的 CI 环境直接 SKIP。**方向：** 新增 `tests/fixtures/gen_fixture.cpp` helper，用 libavcodec（已链入）编码 10 帧 MPEG-4 Part 2 → MP4，调用一次生成 determinism fixture。`tests/CMakeLists.txt` 的 `find_program(ffmpeg)` 分支删掉或降为 fallback。Milestone §M1-debt，Rubric §5.2 + §5.3。
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
