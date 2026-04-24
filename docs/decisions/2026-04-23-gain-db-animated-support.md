## 2026-04-23 — gain_db 动画化：Clip::gain_db → AnimatedNumber + AudioMixer per-frame evaluate_at (Milestone §M3 · Rubric §5.1)

**Context.** `transform-animated-support` bullet 去年轮次收尾时留了最后一段未完成：audio-gain 动画。Transform 的 8 个字段已迁到 `AnimatedNumber`（通过 `transform-animated-integration` 和 `animated-transform-e2e`），但 `me::Clip::gain_db` 仍是 `std::optional<double>`，loader 走 `parse_animated_static_number` 拒绝 `{"keyframes":[...]}` 形式。TIMELINE_SCHEMA.md 早已把 `gainDb` 标注为 "animated number"——schema 文档和实装当前不一致。

AudioMixer 侧的阻碍更深一点：gain 在 `AudioTrackFeed` ctor 时冻结为 `float gain_linear` 常量，`pull_next_processed_audio_frame` 对每个解码帧原位乘这个常量。整个 mixer 没有 timeline T 概念——samples 就流过去了。要做 animated gain 必须换 mixer 抽象：把 gain 从 feed 搬到 mixer，mixer 追一个 emission cursor，每次 `pull_next_mixed_frame` 把 cursor 换算成 `me_rational_t T` 交给 `AnimatedNumber::evaluate_at(T)`。

本轮闭合这条线索，同时把原 bullet 最后一块剥完。

**Decision.** 四层联动迁移：

1. **IR 迁移** — `src/timeline/timeline_impl.hpp` `Clip::gain_db` 从 `std::optional<double>` 换成 `std::optional<AnimatedNumber>`。docstring 更新到 "animated gain in decibels"。
2. **Loader 迁移** — `src/timeline/timeline_loader.cpp` `c.gain_db = parse_animated_number(clip["gainDb"], ...)`（之前 `parse_animated_static_number`）。`parse_animated_number` 已在先前 `animated-number-loader` cycle 里落地并带完整 schema 覆盖（kfs 排序 / interp 枚举 / bezier cp 验证），直接 plug-in。`src/timeline/loader_helpers.hpp` 对应 docstring 从"currently unused"更新到"consumed by parse_transform + Clip::gain_db"。
3. **Track feed 瘦身** — `AudioTrackFeed::gain_linear` 字段删除；`open_audio_track_feed` 签名去掉 `gain_linear` 参数；`pull_next_processed_audio_frame` 内 `apply_gain_fltp` 调用删除；局部 `apply_gain_fltp` 函数整体删除。feed 现在语义干净："decode + resample to target format"。
4. **Mixer 接 T** — `AudioMixer::TrackState` 新增 `me::AnimatedNumber gain_db`；`add_track` 签名加 `gain_db` 参数；类新增私有 `int64_t samples_emitted_` cursor；`pull_next_mixed_frame` 在 mix_samples 调用前，对每 track 算 `T = {samples_emitted_, cfg_.target_rate}` → `gain_db.evaluate_at(T)` → `db_to_linear` → `gains[i]`，把 gains 传给 `mix_samples`（它本就有 `gain_linear` 参数，之前一直传 1.0f）；每帧发出后 `samples_emitted_ += frame_size`。`build_audio_mixer_for_timeline` 把 `clip.gain_db.value_or(AnimatedNumber::from_static(0.0))` forward 到 `add_track`。

**Gain 粒度.** 每个 emitted frame (1024 samples @ 48kHz = ~21ms) 内 gain 保持常量，即 T 取 `samples_emitted_ / rate` 而非逐 sample 插值。理由：(a) DAW 里 envelope automation 的 buffer-level stepping 是工业标准；(b) 保留 `mix_samples` 现有签名不动（该函数 per-sample 输入 gain 数组会让热路径变慢，暂无需求）；(c) 1024/48k 的 step 在人耳层面听不到（典型 envelope 变化率远低于 48Hz ≈ 每帧调一次的速率）。真有 zipper noise 投诉时再演进成 per-sample 插值——届时 `mix_samples` 签名改为 `gains_per_sample[][]` 或 mixer 做 linear ramp 跨帧。

**Timeline-T 约定.** Keyframe `t` 值视为 timeline-global rational time，与 Transform 一致（`compose_sink.cpp` 的 `clip.transform->evaluate_at(T)` 传入的 T 就是 `fi/fps` 这个 global 时间）。mixer 的 `samples_emitted_` cursor 从 mixer 构造（对应 exporter 启动）开始计数，对当前 phase-1 audio（audio track 从 timeline T=0 开始）== timeline-global T。未来支持 audio clip 带 `timeRange.start > 0` 的场景时，mixer 需要加 silence-emit 前置 + 按 `time_start` 在 feed 级做 seek；但那是 audio-mix-scheduler 的事，不在本 bullet。

**Determinism.** `samples_emitted_` 是 int64 counter，单调递增；T 用 `me_rational_t{int, int}` 构造避免浮点。`evaluate_at` 内部走的是 `AnimatedNumber` 既有 deterministic kernel（linear/bezier/hold/stepped 四种 interp 都无浮点积累，只做 per-call 有限位浮点算术）。同 JSON 同 frame_size 配置下，相同 frame_idx 产生相同 `gains[i]`，结合 `mix_samples` 和 `peak_limiter` 的既有 deterministic 性（单线程 IEEE-754，无 FMA 无 SIMD），VISION §5.3 的"软件路径字节可复现"保持。现有 `test_audio_mixer` 的 "synthetic sine-wave mix is bit-identical across two runs" 继续绿。

**Alternatives considered.**

1. **Feed 保留 gain 但每 pull 接受 T 参数** — 拒：feed 不该感知 timeline 时间轴，它是一个 per-track 的 "decode + resample" 纯管线。让 feed 知道 T 会把 compose/mixer 的坐标系注入到一个更底层的抽象，未来加 audio cache / precompute 时此边界会反复被撞。mixer 本来就是 timeline-aware 层（N track → 1 output stream），gain 归它更匹配层次。
2. **Per-sample 线性插值 gain（`mix_samples` 改签名）** — 拒：当前没有 zipper noise 的证据，改 `mix_samples` 的 hot loop 从标量 `gain_linear[i]` 升到 `gains[i][j]` 会把 inner-loop 从纯乘累加变成额外 indirection，且 `mix_samples` 被 2 处单元测试 pinned（2-track mix 和 synthetic sine determinism）——它们都走 scalar gain，升签后都要迁，收益 / 扰动比不划算。留给"zipper-noise observed"专门 bullet。
3. **Mixer 内用 `double seconds` 做 T** — 拒：踩 CLAUDE.md 硬规 "时间是 rational"。`samples_emitted_ / target_rate` 自然是精确的有理数。业界共识（FFmpeg `AVRational`、DaVinci Resolve 内部 rational time）就是这样做。
4. **先加独立 bullet `gain-db-animated-support` 再做** — 拒：原 bullet 明确写 "可考虑独立 bullet `gain-db-animated-support`"，但整体改动 ~300 LOC / 单 cycle 可闭环。分 bullet 意味着先写代码后 commit 两次（IR 迁 + mixer 迁）——迁完 IR 不挂 mixer，audio 会 broken（或 feed 继续用 static value_or）。一次提交把链接补完更干净。
5. **保留 gain_linear 字段作 "legacy" 路径（mixer new 路径 + feed old 路径共存）** — 拒：双写路径是未来几轮 cycle 的隐形 cost。feed 已经没有其他 gain 消费者（所有 add_track 通过 mixer），直接删干净。

**Coverage.**

- `tests/test_audio_mixer.cpp` 新 TEST_CASE "AudioMixer: animated gain_db interpolates linearly across emitted frames"：gain 从 0dB @ T=0 线性降到 -20dB @ T=2048/48000；inject 常量 0.4 2 帧；assert 第 1 帧 ≈ 0.4（0dB），第 2 帧 ≈ 0.4 × 0.31623 ≈ 0.1265（-10dB linear mid），严格单调减小。这是 animated-gain 端到端 tripwire——迁移前在 feed 级无法表达。
- `tests/test_timeline_schema.cpp` 新 TEST_CASE "audio track with audio clip + keyframed gainDb loads into IR"：`{"keyframes":[...]}` 形式 gainDb 解析成功 + `evaluate_at(mid) == -10dB`。pins loader 路径新接受的形式。
- 现有 `audio track with audio clip + static gainDb loads into IR` 迁移 assertion（从 `*gain_db == -6.0` 到 `gain_db->static_value == -6.0`）——continue 覆盖 static 形式不 regress。
- `tests/test_audio_track_feed.cpp` 老 "applies gain on FLTP target" case 删除（feed 不再有 gain 职责）；老 gain_linear 字段 assertion 删除；所有 `open_audio_track_feed` call 更新到新签名。
- `tests/test_audio_mixer.cpp` 老 case 全部更新 `add_track` 签名（6 处），保留 determinism / peak_limiter / sine-mix / inject 验证路径 continue 绿。
- 所有 31 个 ctest 测试套件 Release + Debug + `-Werror` 三种构建下全绿；`01_passthrough` 样例端到端 + ffprobe 验证产出 h264 + aac MP4 不 regress。

**License impact.** 无新依赖。

**Registration.** 无注册点变更：
- C API surface：不变（`me_timeline_load_json` 的 JSON 现在接受更多 gainDb 形式，ABI 层面兼容）。
- Kernel / codec / factory 注册表：不变。
- CMake：不变。
- JSON schema：`gainDb` 在 TIMELINE_SCHEMA.md 早已列为 animated number；本轮让 loader 实装追上 schema 文档，无 schemaVersion bump 需要（loosening acceptance，back-compat）。

**§3a self-check.** 10 条全绿：typed params（无 map）✓、rational time（`me_rational_t{int, int}`）✓、公共头无污染（只改 `src/`）✓、无 extern "C" 越界（mixer internal）✓、无 GPL（无新 dep）✓、确定性（int counter + rational T）✓、stub 不净增（零新 `ME_E_UNSUPPORTED`）✓、无 GL ✓、schema 兼容（loosening accept）✓、ABI 不破（无 header 改）✓。

**§M 自动化影响.** Current milestone M3。M3 exit criterion "所有 animated property 类型的插值正确（linear / bezier / hold / stepped）"已在上一轮（`animated-transform-e2e`，91f30c5）打勾，本 cycle 不改 §M.1。gain_db 是该 exit criterion 的受益方之一（同一 `AnimatedNumber` 内核）但未列独立 exit criterion，不触发新 tick。
