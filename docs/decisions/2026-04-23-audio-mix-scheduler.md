## 2026-04-23 — audio-mix-scheduler（scope-A of audio-mix-scheduler-wire：AudioMixer class）（Milestone §M2 · Rubric §5.1）

**Context.** Bullet `audio-mix-scheduler-wire` 的 4 个 sub-scope：
1. AudioMixer class（N feed → 1 mixed AVFrame stream）
2. H264AacSink audio path 重构，改调 mixer
3. Exporter audio-only track gate 翻
4. 2-track e2e determinism 测试

Per-track feed (`AudioTrackFeed`) 上 cycle 落地。本 cycle 做 sub-scope (1)——把 N 个 feed 聚合成一个 mixer，采样 N 个 stream 到统一 `frame_size` 样本（AAC encoder 的 1024 样本单位），per-channel 经 `mix_samples` 相加、`peak_limiter` 压，emit 固定尺寸 AVFrame。

Before-state grep evidence：
- `grep -rn 'class AudioMixer\|AudioMixScheduler' src/` 返回空——没有 mixer class。
- `src/audio/track_feed.hpp:82` 的 `pull_next_processed_audio_frame` 返回 AVFrame，但 nb_samples 由 decoder 决定（AAC LC 固定 1024；其它 codec 不固定）。下游 AAC encoder 需要固定 1024 的输入；mixer 层必须做 "凑 frame_size" 的 FIFO。
- `src/audio/mix.hpp` 的 `mix_samples` 签名要求 per-channel 单-channel strips：mixer 要做 per-plane 展开调用。

**Decision.**

1. **`src/audio/mixer.hpp/cpp`** 新增 `AudioMixer` class：
   - `AudioMixerConfig`：`target_rate` / `target_fmt`（必须 FLTP）/ `target_ch_layout` / `frame_size`（samples per emitted frame）/ `peak_threshold`（默认 0.95）。
   - `AudioMixer(cfg, err)` ctor：validate FLTP + 非零 rate/frame_size/channels；`ok()` 返回 state。
   - `add_track(feed, err)`：要求 feed 的 target 参数与 mixer config 匹配（rate / fmt / channel count）——mismatch → ME_E_INVALID_ARG。Feed move-in。
   - `pull_next_mixed_frame(out_frame, err)`：
     - Per-track FIFO (`std::vector<std::vector<float>>` 按 channel plane 索引)。
     - For each track：持续调 `pull_next_processed_audio_frame` 追加 samples 到 FIFO，直到 FIFO >= `frame_size` 或 feed EOF。
     - 构造 per-channel inputs (`const float* const*`)，每 plane 调 `mix_samples(inputs, gains={1.0f,...}, N, frame_size, out)`；`peak_limiter(out, frame_size, threshold)`。
     - 新 alloc AVFrame (rate, fmt, ch_layout, nb_samples=frame_size)，每 plane 写入 output，emit。
     - Pop `frame_size` samples（或少于 if FIFO 短）from each plane FIFO。
     - 全部 feed EOF + 全部 FIFO 空 → `ME_E_NOT_FOUND`。
   - **Partial-drain semantic**：一些 track 提前 EOF 时，其 FIFO 残余 < frame_size 部分用 silence (zero) 补齐——mixer 继续 emit 直到最后活跃 track EOF。这保证输出时间线到最长 track 的末尾，不被短 track 截断。
   - **Gain 分工**：`mix_samples` 里 gain 传 1.0（`AudioTrackFeed` 已在 `pull_next_processed_audio_frame` 里应用 per-clip gain_linear）。Mixer 只做 sum + limit，不再二次乘 gain。
   - Move-only（持 `std::vector<AudioTrackFeed>`）。

2. **Scope 边界**：
   - **不** wire 进任何 sink（H264AacSink 的 audio path 还是 passthrough from demuxes[0]）——sub-scope (2)。
   - **不**翻 Exporter 的 audio-only-track gate——sub-scope (3)。
   - **不**写 2-track e2e determinism 测试——sub-scope (4)（需要 scheduler + sink 都就位）。
   - **不**支持 non-FLTP 目标（mix_samples 要求 plane-major float；后续若要 S16/S32 输出，mixer 内加一道 resample 或让 caller 在 mixer 外 post-process）。
   - **不**实现 cross-fade / 动态 gain 自动化（AudioTrackFeed 的 gain 是 per-clip 静态，无 keyframe 动画——M3 animated-support 再做）。
   - **不**实现 ring buffer 优化（FIFO 用 `std::vector::erase(begin(), begin()+take)` 是 O(N) pop；1024-sample window 下可以忍，真成瓶颈再换）。

3. **Tests** (`tests/test_audio_mixer.cpp`) —— 6 TEST_CASE / 98764 assertion（每 mixed frame 1024 sample × 多 frame × 2 track 的 silence 检查是 assertion 主力）：
   - Non-FLTP target → ok() = false + err "FLTP"。
   - Zero frame_size → ok() = false。
   - No tracks added + pull → ME_E_INVALID_ARG。
   - 1-track silent fixture：drain 完整 sequence，每 frame 1024 samples = 0.0 per sample；`eof()` = true 最后。
   - 2-track silent fixtures：两独立 demux 从同一 fixture 开 feed，mixer sum 两 silence → silence；frame_size / format / eof 保持一致。
   - Feed 目标 rate mismatch → add_track ME_E_INVALID_ARG + track_count=0。

**Alternatives considered.**

1. **Frame-by-frame mix without FIFO（要求所有 feed 产出同 nb_samples）** —— 拒：AAC frame_size 是固定 1024，但如果未来混入 MP3（1152 sample）或 Opus（可变），同步失效。FIFO 一致处理所有 codec 粒度差异。
2. **Ring buffer 代替 std::vector::erase** —— 拒：phase-1 不是性能瓶颈（1024 sample × 浮点 = ~4KB/frame × 可能几百 frame），过度优化。有测量数据再做。
3. **Mixer 管理 gain / 让 feed 不做 gain apply** —— 拒：AudioTrackFeed 当前是"per-track 完整 pipeline (decode→resample→gain)" 的抽象单位，移 gain 出去破坏封装。Mixer 的 `gains=1.0` 调用是 mix_samples 签名决定的通用能力（例如未来 crossfade 时自动化）；不直接用时透明 no-op。
4. **pull 接口改成 "pull N samples" 而非 "pull 1 mixed frame"** —— 拒：下游消费者（AAC encoder）要固定 1024 单位输入，"固定 frame_size" 的接口更直接。如果未来要 streaming 混音（流式 N samples），可以加一个 `pull_n_samples` overload，不影响现 API。
5. **把 AudioMixer 跟 H264AacSink 同一个 cycle wire 完** —— 拒：sub-scope (2) 要改的 audio path 是 H264AacSink / reencode_audio 路径里的 "从 demuxes[0] 读 audio" 部分（`src/orchestrator/reencode_audio.cpp` / `reencode_pipeline.cpp`），影响面较大；与 mixer 自身解耦做单独 cycle。
6. **把 peak_limiter 调用位置放 per-sample 或 per-block** —— 当前 per-channel-plane 调 `peak_limiter(strip, frame_size, threshold)`，与 `mix_samples` 同 scope 粒度。跨 channel 做峰值限制（保持立体平衡）是更进阶的做法，phase-1 独立 per-plane 更简单。

**Scope 边界.** 本 cycle **不**做：
- H264AacSink / 任何 sink 的 audio path 重构 → sub-scope (2)。
- Exporter 的 "standalone audio tracks not yet implemented" gate 翻转 → sub-scope (3)。
- 2-track e2e determinism 测试 → sub-scope (4)。

Bullet 保留，narrow 文本反映 sub-scope (1) 已完。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 27/27 suite 绿（+1 suite `test_audio_mixer`）。
- `test_audio_mixer` 6 case / 98764 assertion（silence 的 per-sample check 累计起来很大）。

**License impact.** 无。

**Registration.**
- `src/audio/mixer.hpp/cpp`：新文件，~120 / 180 LOC。
- `src/CMakeLists.txt`：加 `audio/mixer.cpp` 到 media_engine sources。
- `tests/test_audio_mixer.cpp`：新文件，~200 LOC。
- `tests/CMakeLists.txt`：新 test suite + include/libav link + fixture dep + `ME_TEST_FIXTURE_MP4_WITH_AUDIO` define。
- `docs/BACKLOG.md`：bullet `audio-mix-scheduler-wire` 文本 narrow —— 把 AudioMixer class 列入 prereq done 清单，剩余 sub-scope (2)(3)(4)。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **未满足**——需要 sink 接入 + Exporter gate + e2e 才 tick。§M.1 不 tick。
