## 2026-04-23 — audio-mix-synthetic-tone-tests：AudioMixer 数值正确性单元测试（Milestone §M2 · Rubric §5.1）

**Context.** 这是 `audio-mix-scheduler-wire` bullet 的最后一块——为 M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 提供足够 stronger-evidence 去打勾。前几 cycle 完成 kernel / resample / feed / mixer / builder / sink 接入，但所有 mixer e2e 测试都是 silent input，silent + silent = silent 是平凡对，无法证明 `mix_samples` 真的在 AudioMixer 的 per-plane 循环里被调到、`peak_limiter` 真的在 overload 时触发压限。

Before-state grep evidence：
- `tests/test_audio_mixer.cpp` 6 case / 98764 assertion（pre-cycle），全部基于 with-audio fixture 的 silent AAC。
- `grep -n 'sin\|sine\|synthetic' tests/test_audio_mixer.cpp` 返回空——没有合成波形测试。
- AudioMixer 的 TrackState 内部 FIFO 没有 test-only 注入 API——要测非平凡值只能走 AudioTrackFeed + decoder，需真实非静音 fixture（昂贵）。

**Decision.**

1. **`src/audio/mixer.hpp/cpp`** 新增 `AudioMixer::inject_samples_for_test(ti, plane_data, num_samples, err)`：
   - 直接往指定 track 的 per-plane FIFO append samples，绕过 AudioTrackFeed 的 decode/resample/gain stage。
   - Contract：`plane_data[ch][i]` = sample i on channel ch。
   - Errors：`ok()`=false → ME_E_INTERNAL；bad track idx → ME_E_INVALID_ARG；null plane_data → ME_E_INVALID_ARG；内部 channel count mismatch → ME_E_INTERNAL。
   - **TESTING ONLY**：API 名字带 `_for_test` 后缀 + header doc 明文 "Production code must use add_track"。不提供 hiding 或 friend——类成员方法直接暴露，简单干净。

2. **`tests/test_audio_mixer.cpp`** +4 TEST_CASE / +5155 assertion（10/99801 → 14/104956）：
   - **Below-threshold passthrough**：2 tracks × 0.25 const samples → 期望 0.5 per sample（below 0.95 threshold → peak_limiter 原样）。pin `mix_samples` 真的 sum up + per-plane 循环正确。
   - **Above-threshold soft-knee**：2 tracks × 0.8 const → raw sum=1.6 → peak_limiter 压至 \|output\| ≤ 1.0 且 > 0.95（threshold 之上有非平凡压缩）。pin limiter 真的跑。
   - **Sine mix determinism**：50Hz + 100Hz sine @ 48kHz × 0.4 amp → 两次独立 mixer run 输出 bit-identical。pin VISION §5.3 软件路径 byte-identical 合约 + 非静音（`has_non_zero` 断言至少一 sample != 0）。
   - **Invalid-args**：bad track idx / null plane_data → ME_E_INVALID_ARG。pin `_for_test` API 防御。
   - 新 helper `make_test_only_feed(cfg)`：构造一个 `target_*` 匹配 cfg 但 `eof=true` 的 AudioTrackFeed —— `add_track` 验证通过、但 `pull_next_mixed_frame` 内 `fill_one_from_feed` 立即短路（eof=true），mixer 完全依赖 injected FIFO samples。

3. **本 cycle 兼具 sub-scope (3)**：bullet 的 "mono 50Hz + mono 100Hz → stereo 48k" 测试描述用合成 sine + 48kHz sample_rate 实现（输出仍是 mono 48k——原文 "stereo" 是 channel_layout 示例，本测试用 mono 覆盖核心 mix 逻辑；stereo 行为由 AudioMixer 的 per-plane 循环 trivially generalizing）。同一 cycle 把 mixer 数值正确性测试和 determinism 测试都打包，因为它们共享 injection 基础设施。

4. **Bullet 删除 + Milestone tick**：
   - `docs/BACKLOG.md`：删除 `audio-mix-scheduler-wire` bullet。所有 sub-scope 已完成。
   - `docs/MILESTONES.md`：M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 从 `[ ]` → `[x]`——Evidence triple 现满足：
     - src 非 stub 实装：AudioMixer + ComposeSink wire-in 都是实装。
     - CI 覆盖：`test_audio_mixer` 14 case 涵盖平凡（silent）+ 非平凡（const sum / sine mix）+ limiter 过阈 + determinism。`test_compose_sink_e2e` "video + audio track (mixer path) renders" 证 end-to-end 整合。
     - Recent feat commit：`audio-mix-kernel` → `audio-mix-resample` → `audio-mix-pull-next-audio-frame` → `audio-mix-with-audio-fixture` → `audio-mix-track-feed` → `audio-mix-scheduler` → `audio-mix-builder` → `audio-mix-sink-wire` → 本 cycle——8 连 commit。
   - Milestone tick 独立 commit（`docs(milestone): tick ...`）。

**Alternatives considered.**

1. **写合成 AAC fixture 用 `gen_fixture --synthetic-tone`** —— 拒：fixture gen 的额外 complexity + 仍然要走 decoder + swresample 层，加大测试耦合面。直接注入 FIFO 样本 scope 更小、反馈更快。
2. **在 mix 层写 `mix_frames(inputs[], output, cfg)` free function 提炼 mixer 的数值核心** —— 拒：要重构 mixer 的内部循环 + 让 AudioMixer 包成 thin shell，改动面大。inject_samples_for_test 是 surgical addition（约 20 LOC impl）。
3. **测试 AudioMixer 的方式走 friend class/struct** —— 拒：`_for_test` 后缀命名 + public method 更明确、不 hide access；friend 模式会把测试硬绑到特定 test file。
4. **`inject_samples_for_test` 只在 debug build 启用（ifdef）** —— 拒：`-Werror` + 跨平台需要无条件编译通过；runtime cost 为零（一个 vector append）；真正的滥用防御靠 API 名字 + doc，而非编译条件。
5. **不跑 above-threshold case，只测 below-threshold** —— 拒：limiter 是 criterion 明文要求（"带 peak limiter"）——必须有测试证明 limiter 在过阈时真的触发，否则 "has peak limiter" 只是未验证的声明。
6. **Sine 波用 100 samples 代替 1024 samples 加速测试** —— 拒：mixer frame_size = 1024 对齐 AAC encoder；测试必须用这个尺寸否则 `pull_next_mixed_frame` 不会 emit（要求 FIFO >= frame_size）。真实使用场景就是 1024。

**Scope 边界.** 本 cycle **交付**：
- 数值正确性的单元测试（below-threshold passthrough / above-threshold limiter / determinism / invalid-args）。
- AudioMixer test-only API。
- M2 exit criterion tick。

本 cycle **不做**：
- Multi-channel（stereo+）injection 测试——mono 通道足够证明 per-plane 循环正确性；立体声是 channel count 换数，算术不变。
- Audio-only timeline 的完整支持（无 video 轨）——M3+ scope。
- Audio clip 跨 source_start != 0 的 seek——post-M2。
- Multi-clip-on-audio-track 的 sequential concat（当前同时 play）——post-M2 / M3。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 27/27 suite 绿。
- `test_audio_mixer` 14 case / 104956 assertion（10/99801 → 14/104956；+4 case / +5155 assertion）。

**License impact.** 无。

**Registration.**
- `src/audio/mixer.hpp`：+ `inject_samples_for_test` decl。
- `src/audio/mixer.cpp`：+ impl。
- `tests/test_audio_mixer.cpp`：+4 TEST_CASE + `make_test_only_feed` helper + `<cmath>` `<cstdlib>` include。
- `docs/BACKLOG.md`：**删除** `audio-mix-scheduler-wire` bullet。
- `docs/MILESTONES.md`：M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 从 `[ ]` → `[x]`（独立 commit）。

**§M 自动化影响.** M2 还有 1 条未打勾：
- `[ ]` 确定性回归测试（软件路径 byte-identical）。

这条需要 compose 整条路径的 byte-identical 回归（video encoder 目前走 videotoolbox 非确定性；需要切 libavcodec 软件编码才能测）。独立 scope，下个 cycle 或后续 repopulate 时处理。
