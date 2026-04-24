## 2026-04-23 — audio-mix-track-feed（scope-A of audio-mix-scheduler-wire：per-track audio pipeline primitive）（Milestone §M2 · Rubric §5.1）

**Context.** Bullet `audio-mix-scheduler-wire` 的 sub-scope (1) 描述一个 AudioMixScheduler 类要做的事：
> 按 tl.frame_rate 逐 audio frame 从 N audio-track demux 抽 sample 窗口，per-track 经 `resample_to` 转公共 rate/fmt/ch_layout，per-clip `gain_db → db_to_linear` 应用，`mix_samples` 相加，`peak_limiter` 压，产出统一 AVFrame 队列。

这条一下子写 N-track mixer 太重。拆分成两步：
- **本 cycle**：per-track pipeline (`AudioTrackFeed`) ——一个 track 的 decode→resample→gain，独立测试。
- **下一 cycle**：N-track mixer 类 (`AudioMixer`) —— 持有 N 个 feed，每个 "mix 窗口" 从每 feed 拉 samples，经 `mix_samples` 累加、`peak_limiter` 压限，产出统一 AVFrame。

同 video 侧 `TrackDecoderState` → `ComposeSink` 的 scope-A 拆法：per-track 原语先于 scheduler 整合。

Before-state grep evidence：

- `src/audio/` 目前只有 `mix.{hpp,cpp}`（kernel：`mix_samples` / `peak_limiter` / `db_to_linear`）和 `resample.{hpp,cpp}`（one-shot libswresample wrapper）。**没有** 把 demux+decoder+resample+gain 绑在一起的 per-track primitive。
- `grep -rn 'AudioMixScheduler\|AudioTrackFeed' src/ include/` 返回空——本 cycle 前两个概念都不存在。
- Video 侧对应物 `me::orchestrator::TrackDecoderState`（`src/orchestrator/frame_puller.hpp:86`）把 demux + decoder + scratch 绑在一起——audio 侧过去没有这层抽象，直接用 `pull_next_audio_frame` + 手搓 resample 得每用每搭。
- `src/audio/resample.hpp:7` 的 comment 显式说 "AudioMixScheduler will call per audio source per mix window"——设计 intent 早已存在，只等这个 primitive 落地。

**Decision.**

1. **`src/audio/track_feed.hpp/cpp`** 新增：
   - `struct AudioTrackFeed`：持 demux（shared_ptr）+ audio_stream_idx + 解码 AVCodecContext（CodecPool::Ptr） + pkt/frame scratch（AvPacketPtr / AvFramePtr）+ 目标格式（`target_rate`/`target_fmt`/`target_ch_layout`）+ `gain_linear` + `eof` sticky。
   - `open_audio_track_feed(demux, pool, target_rate, target_fmt, target_ch_layout, gain_linear, out, err)`：`av_find_best_stream(AUDIO)` → alloc+open decoder → 分配 scratch → copy target channel layout → 填 out。错误码映射：null demux / 零 target_rate → `ME_E_INVALID_ARG`；无 audio stream → `ME_E_NOT_FOUND`；无 decoder → `ME_E_UNSUPPORTED`；libav 分配失败 → `ME_E_OUT_OF_MEMORY`；open2 / params_to_context 失败 → `ME_E_INTERNAL`。
   - `pull_next_processed_audio_frame(feed, out_frame, err)`：调 `me::orchestrator::pull_next_audio_frame` → 调 `me::audio::resample_to` 转目标格式 → 对 FLTP 调 `apply_gain_fltp(frame, gain_linear)`（in-place 每 plane * gain）→ `*out_frame` = 新分配的 AVFrame（caller `av_frame_free`）。EOF 返回 `ME_E_NOT_FOUND` 并置 `feed.eof = true`。

2. **手动实现 move ctor / move assign**：`AudioTrackFeed` 有 `AVChannelLayout` 成员，需要 `av_channel_layout_uninit` 释放。默认 move 会浅拷贝 heap 数据（某些 layouts 带自定义 `u.map` 堆分配），导致双释。显式 move：先 uninit dst，再 copy src 层到 dst，然后 uninit src。C++20 有 `AVChannelLayout` 只是 POD + 可选堆指针——这是 libav 的 C API 约束——我们只能按其合约走。

3. **Scope 边界（明确不做）**：
   - **不**做 N-track mixer 类（`AudioMixer`/`AudioMixScheduler`）——下一 cycle。
   - **不**改 H264AacSink / Exporter —— audio mix 的 sink 侧重构是 bullet sub-scope (2)(3)，留到 mixer 类就位之后。
   - **不**实现 `pull_n_samples(feed, n, ...)` 这种"凑 N 样本"的 FIFO 版本——按 AVFrame 粒度走（libav 里 audio 的天然单位），未来 mixer 自己做跨帧 accumulate。
   - **不**支持非 FLTP 目标格式的 gain 应用——mixer phase-1 规定 FLTP 工作格式；其它目标直接 passthrough gain=1（decision doc 的明文契约）。
   - **不**做多 clip 跨边界的 sample alignment——一个 feed 只对应一个 (demux, audio_stream) 对。多 clip 的 concat-on-same-track 需要 caller 切换 feed 或上层 mixer 处理。

4. **Tests** (`tests/test_audio_track_feed.cpp`) —— 8 TEST_CASE / 1288 assertion（大部分 assertion 来自 drain 循环 + silence-verify 循环的 per-sample checks）：
   - null demux → ME_E_INVALID_ARG
   - zero target_rate → ME_E_INVALID_ARG
   - video-only fixture (无 audio stream) → ME_E_NOT_FOUND
   - open 成功时字段正确填充（stream_idx / rate / fmt / ch_layout.nb_channels / gain / eof）
   - 完整 drain with-audio fixture 到 EOF，每帧 sample_rate==48000 / fmt==FLTP / channels==1
   - Silent input × gain=0.5 → 所有 samples 仍为 0.0（silence 不变 + 不 NaN/Inf）
   - null out_frame → ME_E_INVALID_ARG
   - Default-constructed feed (未 open) → ME_E_INVALID_ARG

**Alternatives considered.**

1. **把 per-track feed 做进 `TrackDecoderState`（video 侧同一个 struct）** —— 拒：video 的 TrackDecoderState 只管 decode，不管 resample/gain；塞 audio 特有的 `target_ch_layout` + `gain_linear` 会给 video caller 加无用成员。两套 struct 对应两套 semantic（video: "下一帧原始像素"；audio: "下一帧目标格式+gain-applied samples"）更清晰。
2. **把 feed 设计成"拉 N samples"粒度而非 AVFrame** —— 拒：(a) libav audio decoder 输出的自然单位是 AVFrame；(b) N-samples FIFO 要维护 cross-frame 状态（partial frame 剩余），是 mixer 的职责，不是 per-track feed 的；(c) AVFrame 对 encoder 侧同样是自然单位（AAC encoder frame_size 固定 1024，caller 凑够 emit）。
3. **用 shared_ptr<AudioTrackFeed> 避免 move 复杂度** —— 拒：per-call allocation + pointer indirection 带来的性能和 API 复杂度不值；`AVChannelLayout` 的 move 处理是一次性成本。
4. **不缓存 SwrContext，每帧新建** —— 暂保留现状（resample_to 每 call 新建 SwrContext）。Decision doc 显式把 "per-call swr setup 是否太贵" 留给未来 profile。如果 profile 显示是热点，改 resample_to 或 feed 持久化 SwrContext。本 cycle 不 premature optimize。
5. **一次 cycle 做 mixer + feed** —— 拒：scope 爆炸。mixer 的 mix-window cadence、sample alignment、encoder frame 凑齐是独立 scope。feed 单独立起来 + 独立测试 + 下一 cycle mixer 组合它们，一次改一层。

**Scope 边界.** 本 cycle **不**做：
- `AudioMixer` class（mixer across N feeds）。
- ComposeSink / H264AacSink audio path 改造。
- Exporter audio-only track gate 翻转。
- 2-track audio mix e2e determinism 测试。

`audio-mix-scheduler-wire` 的 4 个主 sub-scope 全部未完；**bullet 不删**。narrow 文本把 "AudioTrackFeed 落地" 列入 prereq 清单。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 26/26 suite 绿（+1 suite `test_audio_track_feed`）。
- `test_audio_track_feed` 8 case / 1288 assertion。

**License impact.** 无。

**Registration.**
- `src/audio/track_feed.hpp/cpp`：新文件，~180 / 200 LOC。
- `src/CMakeLists.txt`：加 `audio/track_feed.cpp` 到 media_engine sources。
- `tests/test_audio_track_feed.cpp`：新文件，~180 LOC。
- `tests/CMakeLists.txt`：新 test suite + include/libav link + fixture deps + 两 ME_TEST_FIXTURE_MP4 define。
- `docs/BACKLOG.md`：bullet `audio-mix-scheduler-wire` 文本 narrow——把 "per-track AudioTrackFeed primitive" 列入已就位 prereq；sub-scope (1) 的 "per-track feed 部分" 标记 done，剩余 mixer 聚合 + sink/Exporter 接入 + e2e 测试未动。
- **不**删 bullet（sub-scope 2/3/4 未做 + sub-scope 1 只完成 per-track 部分）。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **未满足**——需要 mixer + sink 接入才 tick。§M.1 不 tick。
