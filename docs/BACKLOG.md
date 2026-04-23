# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M1 收尾或 M2 起步）

- **debt-test-timeline-segment** — `timeline::segment()` 是 orchestrator 按时间切片的核心（每个段 compile 一个 Graph），但只有 `examples/03_timeline_segments/main.cpp` 的 print-to-stdout 验证，没有 doctest 覆盖。重构 segment 边界算法时没有 tripwire。**方向：** `tests/test_timeline_segment.cpp`：hand-build 带 2 clip 单轨的 Timeline（通过 `TimelineBuilder`），调 `timeline::segment()`，断言 (a) segment 数 == clip 数、(b) 每个 segment 的 `time_range` 匹配 clip.timeRange、(c) 单 clip 边界不 artificial break。Milestone §M1-debt，Rubric §5.2。
- **ocio-pipeline-factory** — `ocio-integration-skeleton` 落了命名空间和 CMake 入口，但没有 factory：callers 还得自己 `new IdentityPipeline`。未来 OCIO 上线时换 pipeline 实现要改所有 callsite。**方向：** `src/color/pipeline.hpp` 新增 `std::unique_ptr<Pipeline> me::color::make_pipeline()`——当 `ME_HAS_OCIO` 定义则返回 `OcioPipeline`（目前还没写，该分支先不编）否则返回 `IdentityPipeline`。所有 future callsite 调 factory，OCIO 上线时只改一处。Milestone §M2-prep，Rubric §5.1。
- **docs-testing-conventions** — `tests/` 现在 9 个 suite + 1 个 fixture generator + `ME_TEST_FIXTURE_MP4` 约定 + skip-when-HW-unavailable 模式（test_determinism reencode case），但没有单一文档讲"写新 test 应该怎样走 fixture 复用 / skip 策略 / doctest 习惯"。**方向：** 新 `docs/TESTING.md`：fixture 复用（`add_dependencies(... determinism_fixture)`）、`skip when fixture missing` / `skip when HW unavailable` 两套模板、doctest `REQUIRE` vs `CHECK` 用法、`ME_TEST_FIXTURE_MP4` injection。Milestone §M2-prep，Rubric §5.2。
- **debt-timeline-loader-engine-seed-pattern** — `me_timeline_load_json`（`src/api/timeline.cpp`）在 loader 返回后循环灌 `engine->asset_hashes`。loader 自己 engine-agnostic（`me::timeline::load_json` 只吃 JSON），结果 extern "C" 入口同时扮演"薄胶水"和"业务副作用触发点"两份角色；pattern 不扩展。**方向：** 等 M2 第二种 seed 资源（color pipeline / effect LUT 预热）出现再定两条路：(a) `me::timeline::load_json` 吃 `Engine*`，牺牲 engine-agnostic；(b) Timeline 暴露 `apply_to_engine(Engine&)` hook，保持 loader 纯净。Milestone §M2-debt，Rubric §5.2。
- **debt-timeline-builder-negative-tests** — `tests/timeline_builder.hpp` 的 fluent TimelineBuilder 被三个 test suite 消费（test_determinism / test_cache / test_probe-to-be），但所有用法都是 positive case——没有 negative scenario 覆盖"错误 schemaVersion"、"unknown codec name"、"负 duration"等 loader 应该 reject 的 JSON。**方向：** `tests/test_timeline_schema_negative.cpp`（或扩 `test_timeline_schema.cpp`）：TimelineBuilder 生成 schemaVersion=2 / schemaVersion 缺失 / duration.den=0 / assets 空 / clip 引用未知 assetId，每个断言 `me_timeline_load_json` 返回非 OK + err 串有语义 token。Milestone §M2-prep，Rubric §5.2。
- **debt-test-cache-invalidate-coverage** — `tests/test_cache.cpp` 断言 `me_cache_stats` 随 asset 插入递增，但没断言 `me_cache_invalidate_asset` 的反向语义（invalidate → stats 回退）。invalidate 现在是 impl 完了的（`a4a1c1c`），应该覆盖。**方向：** test_cache.cpp 加一个 case：load timeline → stats.entry_count ≥ 1 → `me_cache_invalidate_asset(eng, asset_id)` → stats.entry_count 减少 1。Milestone §M1-debt，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **multi-track-video-compose** — 只支持单轨。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。依赖 timeline-asset-map（多 track 共享 asset）+ output-sink-interface（合成后走单一 encode path）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **transform-static** — `Transform`（translate/scale/rotate/opacity）是 M2 exit criterion，但 timeline schema / graph / compose 路径没有任何 Transform 概念。**方向：** timeline schema 加 `clip.transform` 可选对象（`{translateX: rational, scaleY: rational, rotation: rational_degrees, opacity: 0..1}`），orchestrator compose 路径（待 multi-track-video-compose 落地后）消费。Milestone §M2，Rubric §5.1。
- **cross-dissolve-transition** — M2 exit criterion；timeline 里 clip 之间没有 transition 概念。**方向：** timeline schema 加 `track.transitions[]` 数组，每个 transition 描述 `{fromClipId, toClipId, kind: "crossDissolve", duration: rational}`；compose 路径在 overlap 区间做 src/dst alpha 混合。Milestone §M2，Rubric §5.1。
- **codec-pool-real-pooling** — CodecPool 目前只跟踪 live_count，不做真 reuse（见 codec-pool-impl 决策：FFmpeg encoder 跨 stream 不安全）。真正能 pool 的是 decoder（`avcodec_flush_buffers` 可复位状态）。**方向：** 等 `reencode-multi-clip` 的 follow-up（`codec-pool-decoder-reuse`）或 M4 多段音频需要反复开同 codec 时加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` 的 pool 路径。现在是 debt 占位。Milestone §M4-prep，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
