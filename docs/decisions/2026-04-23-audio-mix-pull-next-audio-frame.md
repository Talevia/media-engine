## 2026-04-23 — audio-mix-pull-next-audio-frame（scope-A of audio-mix-scheduler-wire：per-demux 音频帧 pull helper）（Milestone §M2 · Rubric §5.1）

**Context.** `audio-mix-scheduler-wire` bullet full scope：AudioMixScheduler class + per-track audio demux 读 + resample + gain + mix + peak_limit + 喂 encoder + Exporter audio gate flip + e2e。这是多 cycle 工程。本 cycle 切最底层原语：**per-(demux, decoder) 的音频帧 pull** —— video 侧的 `pull_next_video_frame` 的 audio 对称版。AudioMixScheduler 会在 follow-up cycle 组合 N 个这种 primitive。

Before-state grep evidence：

- `src/orchestrator/frame_puller.hpp` 只有 `pull_next_video_frame`——无 audio 对称版。
- `src/orchestrator/reencode_segment.cpp:240-270` 有 inline `av_read_frame → send_packet → receive_frame` state machine 但嵌在 segment-level 循环里，混杂 video + audio 处理；无可复用的 "pull one audio frame" API。

**Decision.**

1. **`src/orchestrator/frame_puller.hpp/cpp`** 新 free function：
   ```cpp
   me_status_t pull_next_audio_frame(
       AVFormatContext* demux, int audio_stream_idx,
       AVCodecContext*  dec,   AVPacket* pkt_scratch,
       AVFrame*         out_frame, std::string* err);
   ```
   - 合约完全镜像 `pull_next_video_frame`：`ME_OK` + frame filled；`ME_E_NOT_FOUND` = clean EOF；`ME_E_DECODE` / `ME_E_IO` / `ME_E_INVALID_ARG`。
   - 实装结构逐行镜像——libav 的 codec state machine (`send_packet` / `receive_frame` / drain) 对 video vs audio 完全一致，只有"哪个 stream_index 算感兴趣" filter 不同。
   - 共用 `src/orchestrator/frame_puller.hpp/cpp` 的 include chain + 同命名空间 (`me::orchestrator`)。
   - **注意**：本 helper 的输入一 stream，输出一 AVFrame。N-track 混音需要 caller 开 N 个 `(demux, pkt_scratch, frame_scratch, dec)` 组合调 N 次——follow-up `audio-mix-scheduler-wire` 把这个组合封成 AudioMixScheduler。

2. **Tests**（`tests/test_frame_puller.cpp`）—— +3 TEST_CASE（6→9 cases；54→59 assertions；+5 assertions 因为 audio 路径 fixtures 无音频时仅 skip-path）：
   - Null args → `ME_E_INVALID_ARG`。
   - 负 stream idx → `ME_E_INVALID_ARG`。
   - Fixture 带 audio 就完整 drain + 断 pulled ≥ 1；否则 MESSAGE skip + 提早 return。现 dev fixture（`gen_fixture.cpp`）无音频——实际跑走 skip 路径。
   - Idempotent EOF 情况不单测——逻辑和 video 对称，video 的 idempotent EOF 测试已验证 drain state machine，audio 共享同样的 libav API，复用 video 的 invariant 覆盖。

**Fixture 音频未来添加**: 当前 `gen_fixture.cpp` 只生成 MPEG-4 Part 2 video，无 audio stream。测 audio 路径用 fixture 时得 skip。follow-up `gen_fixture-audio` bullet（未来当需要真 e2e audio 测试时加）：在 fixture 加 silent AAC audio stream——MP4 容器支持、AAC encoder 在 libavcodec 里自带，LGPL-clean。

**Scope 边界.** 本 cycle **不**做：
- AudioMixScheduler class（per-track audio state + sample alignment）。
- ComposeSink / 新 AudioCompSink 的 audio path 接入。
- Exporter audio-track gate flip。
- 多轨 audio e2e 测试（需 2 个音频 fixture）。

以上是 `audio-mix-scheduler-wire` bullet 的剩余部分，follow-up cycle 进行。

**Alternatives considered.**

1. **把 `pull_next_audio_frame` 抽泛型模板** 让 video / audio 共享 impl —— 拒：~40 行代码量，template/overload 开销大于收益；两 helper 合约完全一致，future reader 一眼能对比区别（唯独 stream_index filter 和 semantic 命名）。
2. **把 audio-side state machine 加到 video 里（`pull_next_media_frame(stream_idx, ...)`）** —— 拒：API 混乱——caller 得知道 `stream_idx` 的 `AVMediaType` 不对称；两个名字清晰。
3. **复用 reencode_segment.cpp 的 inline state machine**（factor out 共享 helper） —— 拒：reencode_segment 的 inline state 机 + segment-level logic 紧耦合（mid-loop cancel check + src_end_us 比较等），提炼改动面太大；frame_puller 是纯"给一帧"语义，和 reencode 的"段级 loop"不同抽象层。
4. **给 fixture 加 audio 这 cycle 也做** —— 拒：fixture gen 逻辑需要 AAC encoder 额外的 stream setup + encoder lifecycle——独立 scope，未来用到真 audio e2e 测试时做。
5. **AudioMixScheduler 同 cycle 做** —— 拒：scope 爆炸。per-track sample cursor + 跨 clip 边界 decode 切换 + gain + mix + limiter 打包是几百 LOC 工作；单 cycle 做不完。

业界共识来源：FFmpeg `doc/examples/decode_audio.c` 的 drain pattern 和 `decode_video.c` 完全一致——audio/video decode state machine 是同构的。本 cycle 的两 helper 分名字而非分 impl 仅因为 caller 的调用上下文通常按 media 类型路由。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 25/25 suite 绿。
- `test_frame_puller` 9 case / 59 assertion（先前 6/54；+3 case / +5 assertion）。
- 其他 24 suite 全绿。

**License impact.** 无。

**Registration.**
- `src/orchestrator/frame_puller.hpp/cpp`：+1 free function `pull_next_audio_frame`。
- `tests/test_frame_puller.cpp`：+3 TEST_CASE。
- **无** CMakeLists / BACKLOG / schema 改动——TU 数量不变；bullet 的剩余 scope 未动，只把一个原语从"待写"状态升级到"可用"，本 cycle **不**改 `audio-mix-scheduler-wire` bullet 文本（剩余 scope 依然准确描述未完成部分——scheduler + sink 接入 + Exporter gate + e2e）。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音" 本 cycle **未满足**——scheduler + sink 未接。§M.1 不 tick。
