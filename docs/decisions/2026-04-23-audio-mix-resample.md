## 2026-04-23 — audio-mix-resample（scope-A：libswresample wrapper）（Milestone §M2 · Rubric §5.1）

**Context.** `audio-mix-resample` bullet 总 scope 三块：(1) libswresample 每-source 到公共 rate/fmt/ch_layout 的转换、(2) AudioMixScheduler 按 timeline 时间抽 per-track sample 窗口、(3) sink 重构接混音 AVFrame。本 cycle 切 (1)——把 swr_alloc_set_opts2 + swr_init + swr_convert 的样板封进一个 one-shot wrapper function。同 `audio-mix-kernel` 的 scope-A 方式一致（那 cycle 出的是 pure math，这 cycle 出的是 one-shot resample adapter）。

本 cycle 再次 rotate 过 top P1 `multi-track-compose-actual-composite`——其剩余工作是 architectural integration 的多 cycle 工程，单 cycle 做不完；scope-A slicing 已经榨干。轮到 `audio-mix-resample` 做 kernel-adjacent 单函数 scope-A。

Before-state grep evidence：

- `grep -rn 'swr_\|SwrContext' src/`：`src/orchestrator/reencode_audio.cpp:185, 209` 有 `swr_convert` 的 usage 和 `src/orchestrator/reencode_segment.cpp:155` 的 swr_alloc_set_opts2 + swr_init——但 **in-line 组装**，不复用；`src/io/ffmpeg_raii.hpp:55` 有 `SwrContextPtr`  RAII。
- `grep -rn 'me::audio::' src/` 只命中 `src/audio/mix.{hpp,cpp}` 的三 free function——无 resample helper。

**Decision.** 新 `src/audio/resample.{hpp,cpp}` + `me::audio::resample_to` 函数：

1. **签名**：
   ```cpp
   me_status_t resample_to(const AVFrame* src,
                            int dst_rate,
                            AVSampleFormat dst_fmt,
                            const AVChannelLayout& dst_ch_layout,
                            AVFrame** out,
                            std::string* err);
   ```
   - 输入 `src` 是任意 decoder output（src_fmt / src_rate / src_ch_layout 都从 AVFrame 自己拿）。
   - 输出 `*out` 是新 allocate 的 AVFrame，caller ownership（`av_frame_free`）。
   - 失败模式：null args / 非正 dst_rate → `ME_E_INVALID_ARG`；swr 分配 / init 失败 → `ME_E_INTERNAL`；OOM → `ME_E_OUT_OF_MEMORY`。

2. **Impl 要点**（`resample.cpp`）：
   - `swr_alloc_set_opts2(&raw, &dst_ch, dst_fmt, dst_rate, &src->ch_layout, src->format, src->sample_rate, 0, nullptr)` + 包 `SwrContextPtr` RAII。
   - `swr_init`。
   - `dst` AVFrame: 分配 + `av_channel_layout_copy(&dst->ch_layout, &dst_ch)` + `swr_get_out_samples(swr, src->nb_samples)` 作 `nb_samples` 上限 + `av_frame_get_buffer(dst, 0)` 分配缓冲。
   - 空 src (`nb_samples==0`) → 返回空 dst AVFrame + ME_OK（downstream 按 0-sample frame 跳过）。
   - `swr_convert(swr, dst->extended_data, dst->nb_samples, src->extended_data, src->nb_samples)` → 实际 converted sample count 更新 `dst->nb_samples`（swr 可能产出比 estimate 少的样本，尤其首批）。
   - 每-call 新建 SwrContext；cache 是 follow-up perf。

3. **Tests**（`tests/test_audio_resample.cpp`，9 TEST_CASE / 177 assertion）：
   - Null args（src / out / dst_rate 负值）→ `ME_E_INVALID_ARG` 三个变体。
   - Identity (48k mono S16 → 48k mono S16) → output params 一致；`nb_samples` 在 [120, 128] 窗口（允许 swr 首批 latency）。
   - 48k → 96k upsampling → `nb_samples` 约 2 倍（[400, 600] 对 256 输入）。
   - 48k → 24k downsampling → `nb_samples` 约 half（[100, 160] 对 256 输入）。
   - Mono → stereo upmix → output `ch_layout.nb_channels == 2`。
   - S16 → FLT format change → output 每 sample 在 [-1.01, 1.01] 的 float 范围（S16 值 `i*37 % 30000` 经 swr 缩放到 float）。大量 assertion（每 sample check 两次）——总 177 assertion 的主要源。
   - Empty src (nb_samples=0) → 空 dst + ME_OK。

4. **Follow-up bullet**：删除现版 `audio-mix-resample`，加 `audio-mix-scheduler-wire`——承接 (2)+(3)。scheduler 负责按 timeline 时间抽 per-track sample 窗口 + 调用本 cycle 的 `resample_to` + 调用 `mix_samples` + 送 audio encoder。sink 重构把 H264AacSink 里 audio path 换成调 scheduler 而非 passthrough per-segment。

**Alternatives considered.**

1. **在函数内部 cache SwrContext** 按 (src_fmt, src_rate, src_ch, dst_fmt, dst_rate, dst_ch) key —— 拒：one-shot per-call 契约更简单；cache 是 perf 优化，等 benchmark 证实瓶颈再做。
2. **返回 `SwrContextPtr` 让 caller 复用** 而非每次 alloc —— 拒：API shape 变大，caller 得维护 context 生命周期；违反本 function "一条命令干一件事"原则。
3. **接受 `swr_flush` 作为第二阶段调用**（process + flush） —— 拒：audio-mix-scheduler 可以自己管 flush；本 wrapper 只做 convert。
4. **dst AVFrame 由 caller 预分配** 而非我分配 —— 拒：`swr_get_out_samples` 才知道大概大小；caller 提前分配要重复 `swr_get_out_samples` 调用，浪费。
5. **替 AVFrame 用 raw pointer + sample count** —— 拒：AVFrame 是 libav API 的通用 data shape，encoder 下游就是吃 AVFrame，不引入 API 翻译层。
6. **对齐 `av_frame_get_buffer` 的 align=32** 而非 default 0 —— 拒：`av_frame_get_buffer(dst, 0)` 取 libav 当下对架构合理的默认对齐；hardcode 32 在某些 platform 浪费。
7. **Channel layout 用 int count 而非 `AVChannelLayout`** —— 拒：`AVChannelLayout` 是 libav 5.1+ 的 typed channel API（承载 mono/stereo/5.1 的 complex layouts），使用 int count 会丢信息。

业界共识来源：FFmpeg 源码 `doc/examples/resampling_audio.c` 的 swr_alloc_set_opts2 + swr_convert + dst frame 分配模式，reencode_audio.cpp 的现有 inline usage，libswresample 5.0 API 规范。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 22/22 suite 绿（新 `test_audio_resample` 是第 22 个）。
- `build/tests/test_audio_resample` 9 case / 177 assertion / 0 fail。
- 其他 21 suite 全绿，无 side-effect（新 TU 独立；`reencode_audio.cpp` 的 inline swr usage 不动——本 cycle 不替换 existing call sites，那是 scheduler-wire cycle 的事）。

**License impact.** 无新 dep（libswresample 已在 FFmpeg 里）。

**Registration.**
- `src/audio/resample.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` `media_engine` sources 追加 `audio/resample.cpp`。
- `tests/test_audio_resample.cpp` 新 suite + `tests/CMakeLists.txt` `_test_suites` 追加 + 专用 `target_link_libraries` (FFMPEG::swresample)。
- `docs/BACKLOG.md`：删 `audio-mix-resample`，P1 末尾加 `audio-mix-scheduler-wire`。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **未完成**——数学内核 (kernel) + resample wrapper (本 cycle) 就位，但 scheduler + sink 没接。`src/orchestrator/exporter.cpp` 的 audio track gate 仍 UNSUPPORTED。criterion 保留未打勾。
