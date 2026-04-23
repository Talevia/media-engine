## 2026-04-23 — debt-split-reencode-pipeline-audio-fifo：audio FIFO 逻辑下沉到 reencode_audio（Milestone §M1-debt · Rubric §5.2）

**Context.** `reencode-multi-clip` cycle 让 `src/orchestrator/reencode_pipeline.cpp` 从 300+ 行重新涨到 602 行（scan-debt 2026-04-23 snapshot）。多出来的密度主要在两处 audio FIFO 路径：

1. `drain_audio_fifo()` free function（跨 segments 把 FIFO 按 encoder `frame_size` 切成小块送 encoder + 累计 `next_audio_pts`）。
2. `push_audio_frame` lambda（包 swr_convert + `av_samples_alloc_array_and_samples` + FIFO 写入 + 结尾调 `drain_audio_fifo`）。

两处都靠 captured `SharedEncState` + lambda 的 `fail` helper 访问 encoder / swr / afifo / next_audio_pts 共享状态。这让 `reencode_pipeline.cpp` 有 ~80 行是"怎样让 AAC 吃固定 frame_size"这一个问题的 mechanics，而不是 orchestration。新 codec（ProRes audio、Opus）或新 audio-only 输出路径未来想复用 FIFO 逻辑时要么拷贝这段代码、要么先把它从 captured-state lambda 里解耦——后一步显然应该现在做。

**Decision.** 两个 TU 级函数下沉到 `src/orchestrator/reencode_audio.{hpp,cpp}`（已经有 `open_audio_encoder` / `encode_audio_frame`，audio 相关的三件套收齐到同一个 TU）：

```cpp
namespace me::orchestrator::detail {

me_status_t drain_audio_fifo(
    AVAudioFifo*, AVCodecContext* aenc, AVFormatContext*,
    int out_stream_idx, int64_t* next_pts_in_enc_tb,
    bool flush, std::string* err);

me_status_t feed_audio_frame(
    AVFrame* in_frame, SwrContext*, int src_sample_rate,
    AVAudioFifo*, AVCodecContext* aenc, AVFormatContext*,
    int out_stream_idx, int64_t* next_pts_in_enc_tb,
    std::string* err);

}
```

签名原则：**只吃 primitive + FFmpeg C struct**，不依赖 `SharedEncState` 或 `ReencodeOptions`。`next_pts_in_enc_tb` 用 `int64_t*` 让 caller 的 counter 语义对称——caller 拥有 PTS 状态，helper 只负责读写。跨 segment concat 的"counter 共享不重置"语义直接体现在这个 out-param 上。

原先 `reencode_pipeline.cpp` 的 `push_audio_frame` lambda 现在是一行 wrapper：

```cpp
auto push_audio_frame = [&](AVFrame* f) -> me_status_t {
    return feed_audio_frame(f, swr.get(), adec ? adec->sample_rate : 0,
                             shared.afifo, shared.aenc, shared.ofmt,
                             shared.out_aidx, &shared.next_audio_pts, err);
};
```

End-of-stream 的 final flush 也从 `drain_audio_fifo(shared, true, err)` 变成正常的 7-param call。

**行数变化.** `reencode_pipeline.cpp`：602 → 523（**降 79 行**）。`reencode_audio.cpp`：108 → 233（**升 125 行**，包含两个新函数的完整 impl + 函数头注释）。净增约 46 行，但**分布明显改善**：audio FIFO mechanics 完全从 orchestration 文件里消失，以后加 `reencode-multi-clip` 的 follow-up（不同 ch_layout 混音、fade-in/out、fixed-bitrate VBR 切 ABR）都只碰 audio TU，不会再往 `reencode_pipeline.cpp` 里灌新 lambda。

**Alternatives considered.**

1. **保持 SharedEncState 传入 helper 签名**——拒：`SharedEncState` 是 `reencode_pipeline.cpp` 内部 struct，把它的 header 暴露给 `reencode_audio.hpp` 会让下层依赖上层的数据布局；signature 只吃 FFmpeg primitives 是更干净的分层。
2. **把 `push_audio_frame` 也做成一个类（`AudioFifoDriver`）**——拒：over-engineering。两个自由函数 + 一个 `int64_t*` 参数就能表达跨段状态；不需要构造函数、析构函数、虚函数。等第二个 consumer 出现再考虑升级。
3. **只下沉 `drain_audio_fifo`，保留 lambda 在 pipeline.cpp**——拒：lambda 里的 swr_convert + samples_alloc_array 是跟 drain 同一个关注点的"送进 FIFO"的前半，留一半下沉一半不如一起下沉。
4. **把 `av_samples_alloc_array_and_samples` 包成 RAII 再下沉**——考虑过，暂缓：两个 call site（正常 + flush）用同一模式 `alloc → convert → (write if converted > 0) → free`，抽 RAII 成本 > 收益（都就十几行）。如果 ProRes audio 上来又加第三个 call site，那时再抽。

业界共识来源："shared resource (FIFO) + per-frame feeder" 是 libavcodec 自己 `fftools/ffmpeg_mux.c` 和 FFmpeg muxer helpers 的标准模式——AVFormatContext + AVCodecContext + AVAudioFifo 自由函数签名，用 int64_t out-param 管累计 PTS。和 HandBrake 的 `work_audio.c`、SoX 的 `input.c` 写法都是这一套。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 9/9 suite 绿。
- `05_reencode /tmp/me-multi.timeline.json` 多 clip 回归：产物与 refactor 前**完全一致**——60 h264 frames + 89 aac frames + 2.04s duration（`ffprobe` 对比）。
- 单 clip 也没动 encoder path，隐式覆盖通过 `test_determinism` 的 h264/aac 确定性 case（`debt-render-bitexact-flags` cycle 加的）。

**License impact.** 无新依赖。

**Registration.** 无公共 API / schema / kernel 变更。仅内部 `me::orchestrator::detail::` 新加 2 个函数，`reencode_audio.hpp` 头里 include 多了 `<libavutil/audio_fifo.h>` + `<libswresample/swresample.h>`（本来就 transitive 可见，显式声明为 PRIVATE header 依赖更干净）。
