# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M1 收尾或 M2 起步）

- **debt-test-thumbnail-color-tagged-fixture** — `tests/test_thumbnail.cpp` 五 case 全部基于 `determinism_input.mp4`（gen_fixture BITEXACT-encoded 所有 color tag 均 UNSPECIFIED）；`grep -n 'color\|tagged' tests/test_thumbnail.cpp` 无 tagged fixture case。`me_thumbnail_png` 未来加 color-managed scaling 时，untagged path 正常不保证 tagged path 正常。**方向：** `gen_fixture.cpp` 加 `--color-range tv --primaries bt709`-style 参数或新 `gen_tagged_fixture.cpp`；`tests/test_thumbnail.cpp` 加一 case 断言 PNG tEXt / iCCP chunk 里带色彩标记（如果 impl 保留）或至少 PNG 仍是合法的 IHDR。Milestone §M2-prep，Rubric §5.2。
- **debt-scan-debt-flag-check-per-file** — `tools/scan-debt.sh:150` 的 §7 "repeated add_compile_options" 对同一文件内的 conditional line 也报 false positive（`CMakeLists.txt:35` 和 `:37` 两行，都在顶层 CMakeLists 里，一个 always、一个条件）。scan 输出过两个 bucket 信号（repopulate snapshot 连续 N 次都带这条"每次都查的 flag 就是这俩"的没价值记录）。**方向：** §7 改成 per-file dedupe + 真·跨文件重复才报——同个 flag 在**不同** CMakeLists 重复出现才是 debt。或者改成"whitelist this pair"隐式跳过。Milestone §M2-prep，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **multi-track-video-compose** — 只支持单轨（loader 断言 `tracks.size()==1`）。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。依赖 timeline-asset-map（多 track 共享 asset）+ output-sink-interface（合成后走单一 encode path）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **transform-static** — `Transform`（translate/scale/rotate/opacity）是 M2 exit criterion，但 timeline schema / graph / compose 路径没有任何 Transform 概念。`grep -rn 'Transform\b' src include` 确认无痕迹。**方向：** timeline schema 加 `clip.transform` 可选对象（`{translateX: rational, scaleY: rational, rotation: rational_degrees, opacity: 0..1}`），orchestrator compose 路径（待 multi-track-video-compose 落地后）消费。Milestone §M2，Rubric §5.1。
- **cross-dissolve-transition** — M2 exit criterion；timeline 里 clip 之间没有 transition 概念。**方向：** timeline schema 加 `track.transitions[]` 数组，每个 transition 描述 `{fromClipId, toClipId, kind: "crossDissolve", duration: rational}`；compose 路径在 overlap 区间做 src/dst alpha 混合。Milestone §M2，Rubric §5.1。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6-prep，Rubric §5.2。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进，不在 M2 scope。Milestone §M3-prep，Rubric §5.2。
- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` 已落地，N-segment 各自 `open_decoder()` 独立开新 AVCodecContext（`reencode_segment.cpp:112`）——bullet 以前在等的"第二个 consumer"条件已到。**方向：** `CodecPool::get_or_make_decoder(AVCodecID, const AVCodecParameters&)` 返回共享 `CodecCtxPtr`；`avcodec_flush_buffers` 复位状态跨 segment 使用。encoder pool 依旧不做（跨 stream 不安全）。Milestone §M4-prep，Rubric §5.3。
