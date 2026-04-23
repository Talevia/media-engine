## 2026-04-23 — audio-mix-kernel（scope-A, 数学内核）：mix_samples + peak_limiter + db_to_linear（Milestone §M2 · Rubric §5.1）

**Context.** Top P1 仍然是 `multi-track-compose-sink-wire`，但过去两次尝试都发现它需要跨越 `reencode_pipeline` 的 decoder/encoder/mux 的架构整合（per-track decode state + per-frame compose loop），这不是 nibble-cycle 能消化的工作。继续切 scaffold-only 的 sink 小块 ROI 已经很低（上一 cycle `ocio-colorspace-conversions` rotation 已经证明 rotation 策略更有效）。本 cycle 再 rotate：top P1 `multi-track-compose-sink-wire` 再跳过一次，从 `audio-mix-kernel` 开始切——bullet 文本明标 "估计 3–4 cycle"，本 cycle 是其 scope-A 数学内核部分（和 `multi-track-compose-kernel` 对 `alpha_over` 的处理同构）。

Before-state grep evidence：

- `grep -rn 'src/audio/\|me::audio::' src/` 返回空——无 `src/audio/` 模块；`audio` 作为 namespace 都还没建立。
- `src/orchestrator/reencode_audio.cpp` 有现有 audio encode helpers（不是 mix，是 AAC encoder FIFO plumbing），不涉及多轨混音数学。
- `Clip::gain_db: std::optional<double>` 字段在 IR 里（`audio-mix-two-track` cycle 引入），但无任何 consumer 把它转成 linear 或做 sample mix。

**Decision.** 新 `src/audio/mix.{hpp,cpp}`（+ `src/audio/` 子目录），三个纯数学 free function + `tests/test_audio_mix.cpp` 的单元测试：

1. **`void mix_samples(const float* const* inputs, const float* gain_linear, size_t num_inputs, size_t num_samples, float* output)`**：
   - 按 `output[j] = Σ inputs[i][j] * gain_linear[i]` 逐 sample 混和 N 个单通道 input strip。
   - `num_inputs == 0` 或 `inputs == nullptr` → 写零（silence）。
   - 不做 clip / limit —— 纯数学 sum；超 ±1.0 由下游 `peak_limiter` 处理。
   - 单声道 strip-level API：interleaved channel layout 是上层调度的事，底层 per channel 调一次。
   - 无 FMA / SIMD，IEEE-754 float32 addition 确定性。

2. **`float peak_limiter(float* samples, size_t num_samples, float threshold = 0.95)`**：
   - Soft-knee：`|x| ≤ threshold` 线性 pass-through；`|x| > threshold` 经 `tanh((|x| - T) / (1 - T)) * (1 - T) + T` 压回 ±1.0。
   - 保 zero symmetry（正负分支对称）。
   - threshold 用 `[0.5, 0.99]` 范围 defensive clamp——pathological 输入不崩。
   - Return 值：观察到的 peak（限制前 `max|x|`），便于 UI meter / headroom 统计。

3. **`float db_to_linear(float db)`**：
   - `10^(db / 20)`，`0 dB = 1.0`、`-6 dB ≈ 0.501`、`-20 dB = 0.1`。
   - `-infinity dB → 0.0 exactly`（特判，避 subnormal）。

4. **Tests**（`tests/test_audio_mix.cpp`，15 TEST_CASE / 49 assertion）：
   - `mix_samples` bullet anchor：两路 1.0 + -∞ dB (=0) → 1.0（即一路静音不影响另一路）。
   - `mix_samples` sanity：0.5 + 0.5 unity → 1.0；per-input gain 独立缩放（0.25 + 0.75 → 1.0）；单路 unity = 严格 copy；empty input set 写零；sinusoid 输入两次跑 byte-identical（deterministic）。
   - `peak_limiter` bullet anchor：输入 1.5 / -1.5 / 2.0 / -3.0 压回 `|y| ≤ 1.0` 且 `|y| > threshold`（soft knee 不 hard clip）。below-threshold exact pass-through（0.1 / -0.2 / 0.9 / 0.94）。zero symmetry（正输入 vs 反号同值输入 output 互为相反数）。peak 返回值 = max|x| before compression。threshold clamp（0.1 / 1.5 都 clamp 到 [0.5, 0.99]，结果仍然有界）。
   - `db_to_linear`：0dB exact 1.0、−6dB ≈ 0.5011872、−20dB = 0.1、−∞ = 0.0 exact。

5. **Scope gate — 留给 follow-up cycle**：
   - **libswresample 接线** —— 把 per-clip AVFrame（decoder 出的 planar/interleaved float/S16/S32）resample 到公共 rate + channel layout + float 格式，然后喂给 `mix_samples`。下一 cycle `audio-mix-resample`.
   - **AudioMixScheduler** —— per-output-AVFrame 从 N demux context pull samples，管 per-track sample cursor 和 fade-in/out（类比 `active_clips_at`）。
   - **Sink 接收 mixed AVFrame** —— H264AacSink 或新 AudioCompSink 消费混音后的 AVFrame 而非从单 demux 抽。
   - **2-track e2e determinism 测试** —— 等 sink wire 完再写。
   
   这四件是 `audio-mix-kernel` bullet 剩余 scope（对照 bullet 原文 "libswresample 接线 ... resample ... 逐 sample 相加 ... sink 重构 ... e2e 确定性"；本 cycle 只落 "逐 sample 相加" + "peak limiter" + "dB 转换" 三件数学，bullet 其他部分拆新 follow-up）。

**Alternatives considered.**

1. **直接做 full audio-mix-kernel 一把（mix + resample + sink）** —— 拒：bullet 自评 "3-4 cycle"；压 cycle 风险写不完、决策失焦。和 `multi-track-compose-kernel` 的 scope-A 处理一致。
2. **mix_samples 签名用 `span<float>` 或 `vector<float>&` 代替 raw pointer**—— 拒：raw pointer 让上层（未来 AVFrame-driven scheduler）零拷贝直接喂 decoder output planar data；span/vector 会强制 copy。
3. **用 int32 internal fixed-point 而非 float** —— 拒：VFX/DAW 标准是 float32 mix bus；int32 fixed 是 hardware DSP 约定，不符合 CPU-software path 一致性。
4. **Hard clip 替 soft tanh** —— 拒：bullet 明写 "简单 soft-knee peak limiter"；hard clip 在 overage 大时听感差。tanh soft 是业界默认（Ableton / Logic / Audacity 的 "soft limiter" preset）。
5. **threshold 默认 1.0（即 hard limiter, 完全无 headroom）** —— 拒：bullet 指定 0.95 threshold；留 5% headroom 让 soft-knee 有空间工作。
6. **`-∞ dB → 0` 靠普通 `pow(10, -∞/20)` 依赖编译器** —— 拒：C++ 标准对 `pow(10, -∞)` 返回 0 但实现允许 subnormal；显式 `if (isinf(db) && db < 0) return 0;` 避免这条路径下的 denormal 数。
7. **给 `mix_samples` 加 alignment hint / `__builtin_prefetch`** —— 拒：M2 correctness-first，SIMD / perf 优化留给后续 profile-driven cycle。
8. **把 `db_to_linear` 放 `src/util/` 或头文件 inline** —— 拒：`audio/mix.hpp` 是 audio namespace 的公共入口；`db_to_linear` 是 audio-specific 数学工具（其他 domain 不会用 dB → amplitude 换算），放 audio namespace 最清晰。
9. **单独的 `tanh_soft_knee(float)` helper** —— 拒：抽象层次过细；tanh knee 公式只在 `peak_limiter` 内部用，不值得单独 TU。

业界共识来源：Audio mixing 的 "sum of scaled inputs" 是 DAW (Logic / Ableton / Pro Tools) 的 ground-truth；peak limiter soft-knee 用 tanh/atan 是多个 OSS limiter plugin (RNS-Calf, LSP-Plugins, zam-plugins) 共识；dB ↔ linear 用 `10^(db/20)` 是声学规范。本 impl 全部采纳。

**Coverage.**

- `cmake --build build` + `-Werror` clean（中间踩一次 `unused function` 的 -Werror；删除未用 helper 后通过）。
- `ctest --test-dir build` 20/20 suite 绿（新 `test_audio_mix` 是第 20）。
- `build/tests/test_audio_mix` 15 case / 49 assertion / 0 fail。
- 其他 19 suite 全绿；纯新 TU 不触其他代码路径。

**License impact.** 无新 dep。纯 C++ 数学。

**Registration.**
- `src/audio/mix.{hpp,cpp}` 新 TU + 新 `src/audio/` 子目录。
- `src/CMakeLists.txt` `media_engine` source list 追加 `audio/mix.cpp`。
- `tests/test_audio_mix.cpp` 新 suite + `tests/CMakeLists.txt` 的 `_test_suites` 追加 + include dir。
- `docs/BACKLOG.md`：删 `audio-mix-kernel`，P1 末尾加窄版 `audio-mix-resample`（resample + scheduler + sink 承接）。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **未完全满足**——数学内核就位但 end-to-end 路径（resample + sink + e2e test）仍缺。§M.1 evidence check：`src/orchestrator/exporter.cpp` 的 audio track gate 仍 UNSUPPORTED；该 exit criterion 保留未打勾。跳过 top P1 `multi-track-compose-sink-wire` 继续挂着——下 cycle 可能需要主动规划 dedicated sink-wire attempt（非 nibble 的完整 cycle）。
