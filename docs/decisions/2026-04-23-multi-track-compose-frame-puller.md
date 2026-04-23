## 2026-04-23 — multi-track-compose-frame-puller（scope-A of actual-composite：pull_next_video_frame helper）（Milestone §M2 · Rubric §5.1）

**Context.** 上一 cycle 抽了 `setup_h264_aac_encoder_mux` helper（encoder+mux bootstrap 共享给 future sinks）。剩余 compose loop 还需要一个原语：**per-track "给我下一帧"**。把这部分单独一 cycle 落下来，避免下一个"真的写 compose loop" cycle 同时处理 (a) 设置 + (b) 逐帧 pull 状态机 + (c) 多轨调度 + (d) encoder 对接——风险太大。

Before-state grep evidence：

- `grep -rn 'av_read_frame\|avcodec_receive_frame' src/orchestrator/` 命中 `src/orchestrator/reencode_segment.cpp:240-270`——decode state machine 的 inline 展开，嵌在 segment 处理的大 while-loop 里。没有可复用的 "pull one frame" API。
- `grep -rn 'pull_next' src/` 返回空。

**Decision.** 新 `src/orchestrator/frame_puller.{hpp,cpp}`，一个 free function：

```cpp
me_status_t pull_next_video_frame(
    AVFormatContext* demux,
    int              video_stream_idx,
    AVCodecContext*  dec,
    AVPacket*        pkt_scratch,
    AVFrame*         out_frame,
    std::string*     err);
```

**合约**：
- `ME_OK` → `out_frame` 有新解码 frame，caller 用完 `av_frame_unref`。
- `ME_E_NOT_FOUND` → demux exhausted，decoder 已 drain 干净，无更多帧。**不是 error**，正常 EOF 信号。
- `ME_E_DECODE` / `ME_E_IO` → libav 真错；err 有详情。
- `ME_E_INVALID_ARG` → null / 负 stream idx。

**Impl**（~70 LOC）：标准 libav "drive the state machine" pattern：
1. `avcodec_receive_frame(dec, out_frame)` 先试。
2. `rc == 0` → done，返回 frame。
3. `rc == EAGAIN` → 需要更多 packet，走 step 4。
4. `rc == AVERROR_EOF` → drain 完成，返回 NOT_FOUND。
5. 其他 → DECODE error。
6. 从 demux `av_read_frame(pkt)`。
7. Packet EOF → `avcodec_send_packet(dec, nullptr)` 送 drain 信号，设 `draining=true`，loop back。
8. Packet OK，`stream_index != video_stream_idx` → unref + skip（非视频 packet）。
9. Video packet → `avcodec_send_packet(dec, pkt)`，unref，loop back。

`draining` flag 防止已 drain 之后又读 demux（demux 真实 EOF 后所有后续 receive 都会是 EAGAIN 或 EOF；draining 期 EAGAIN 直接 loop 到 EOF 返回 NOT_FOUND）。

**Tests**（`tests/test_frame_puller.cpp`，4 TEST_CASE / 42 assertion）：
- Null args → `ME_E_INVALID_ARG`（用 null demux + decoder）。
- Negative stream idx with real demux → `ME_E_INVALID_ARG`。
- 完整 drain：打开 determinism fixture（640×480 @ 25fps × 1s），逐帧 pull 直到 NOT_FOUND。断言 pulled 帧数 ∈ [20, 30]（允许 ±5 帧 slack——fixture 实际 ~25 帧，首 packet decoder latency 可吞 1-2 帧）。最后 pulled frame width=640 height=480。每 frame 取后 `av_frame_unref`。
- Idempotent EOF：drain 完成之后再 call 一次，仍然 `ME_E_NOT_FOUND`，不崩。

测试直接用 libav API 构 fixture（avformat_open_input / avcodec_find_decoder / avcodec_open2），不走 engine factory——把 puller 合约跟 engine-level 整合解耦。

**CMake bug 再现**：初版 `target_compile_definitions(test_frame_puller ME_TEST_FIXTURE_MP4=...)` 放在 `_fixture_mp4` 变量定义之前，结果空字符串 expansion → 测试都走 "skip: fixture not available" 路径（只 1 个 assertion 跑）。修：把 `add_dependencies` + `target_compile_definitions` block 移到 `_fixture_mp4` 定义**之后**（同之前 test_render_cancel / test_render_progress / test_render_job_lifecycle 的 pattern）。42 assertions 全跑通。

**Alternatives considered.**

1. **做 `pull_next_audio_frame` 对称 helper** —— 拒：audio 路径 phase-1 仍走 demuxes[0] passthrough（compose 多轨混音是 audio-mix-scheduler-wire bullet 的事）；video-only helper 足够 unblock `multi-track-compose-actual-composite`。audio 原语等 audio-mix 真的需要 multi-track decode 时再加。
2. **把 puller 做成 `TrackFrameSource` class** 持 decoder / demux / scratch pkt/frame —— 拒：state 合约简单（4 指针 arg），class 是 over-engineering；compose loop 可以自己持 per-track state 的 vector + 调 free function。
3. **`ME_E_NOT_FOUND` vs 新加 `ME_E_END_OF_STREAM` status code** —— 拒：新增 ABI-stable enum value 需要 MILESTONES.md 的 release note 节奏；`ME_E_NOT_FOUND` 语义贴合（"这个查询无结果"）且现有 enum 里存在，reuse。
4. **seek 支持** —— 拒：phase-1 simplification "每 track 恰好 1 clip, source_start=0"下不需要 seek。多 clip/track 或 trimmed clip 才需要 seek；留给后续 cycle。
5. **inline 在 compose_sink.cpp**，不做独立 TU —— 拒：独立 TU 可以单元测试（unit-testable against real fixture）；compose_sink 的 process() 会是 300+ LOC，inline decode state machine 会让可读性下降。
6. **Packet skip 别 unref** —— 拒：av_read_frame 返回的 packet 必须 unref，否则 leak。caller 提供的 pkt_scratch 在各 path 都要 unref 干净。

业界共识来源：FFmpeg 官方 example `doc/examples/decode_video.c` 的 drain pattern（send_packet NULL → EOF signal → receive_frame until EOF）是 libav 3.0+ 的 canonical codec API 用法。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 23/23 suite 绿。
- `build/tests/test_frame_puller` 4 case / 42 assertion / 0 fail。
- 其他 22 suite 全绿。

**License impact.** 无新 dep。

**Registration.**
- `src/orchestrator/frame_puller.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` `media_engine` sources 追加 `orchestrator/frame_puller.cpp`。
- `tests/test_frame_puller.cpp` 新 suite + `tests/CMakeLists.txt` `_test_suites` 追加 + lib links + 两段 `target_compile_definitions` / `add_dependencies` 放在 `_fixture_mp4` 定义之后。
- `docs/BACKLOG.md`：`multi-track-compose-actual-composite` bullet direction 再窄化——现在剩余 = 只 ComposeSink::process() 里调 `setup_h264_aac_encoder_mux` + 用 N 个 `pull_next_video_frame` 做每帧compose + audio 用简化 path + e2e。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加" 本 cycle **未完成**——helper 就位但 compose loop 没写。§M.1 不 tick。
