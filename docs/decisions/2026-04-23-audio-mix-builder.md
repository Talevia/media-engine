## 2026-04-23 — audio-mix-builder（scope-A of audio-mix-scheduler-wire：timeline-driven mixer factory）（Milestone §M2 · Rubric §5.1）

**Context.** `AudioMixer` class 上 cycle 落地，但它需要 caller 手动 open N 个 AudioTrackFeed 并 add_track。下一步 sub-scope (1) —— H264AacSink / ComposeSink 的 audio 路径重构——调用方需要一个"从 Timeline + 已有 demux 映射直接构造 mixer"的方便 builder。本 cycle 切这条 prereq：纯 factory helper，带完整单元测试，不改任何 sink。

Before-state grep evidence：
- `grep -rn 'build_audio_mixer\|make_audio_mixer' src/` 返回空——没有 timeline-driven mixer 构造 helper。
- ComposeSink / reencode_pipeline / H264AacSink 的 audio 路径都是从 `demuxes[0]` 的 audio stream 直接 passthrough + reencode（`src/orchestrator/reencode_audio.cpp`）——要换成 mixer driven 得先有 mixer factory。
- `src/audio/mix.hpp:75` 的 `db_to_linear(float)` 函数存在；`me::Clip::gain_db` 字段是 `std::optional<double>`（`src/timeline/timeline_impl.hpp` 的 Clip struct）。factory 需要把 `clip.gain_db.value_or(0) → db_to_linear → feed.gain_linear`。

**Decision.**

1. **`src/audio/mixer.hpp/cpp`** 新增 `build_audio_mixer_for_timeline(tl, pool, demux_by_clip_idx, cfg, out, err)`：
   - 扫 `tl.clips`，过滤 `kind_for_track_id(c.track_id) == TrackKind::Audio`。
   - 每 audio clip：`gain_linear = db_to_linear(clip.gain_db.value_or(0))`，调 `open_audio_track_feed(demuxes[ci], pool, cfg.target_rate, cfg.target_fmt, cfg.target_ch_layout, gain_linear, feed, err)`。
   - `mixer.add_track(std::move(feed))`。
   - 返回 `std::unique_ptr<AudioMixer>` via `out`。
   - Error map：
     - `demux_by_clip_idx.size() != tl.clips.size()` → `ME_E_INVALID_ARG`
     - `tl.clips.empty() || tl.tracks.empty()` → `ME_E_INVALID_ARG`
     - `audio_clips_found == 0` → `ME_E_NOT_FOUND`（清晰区分于"timeline 有 audio 但 demux 问题"）
     - `demuxes[ci] == null` for an audio clip → `ME_E_INVALID_ARG` + clip idx 前缀
     - `open_audio_track_feed` / `add_track` 失败 → 透传并 prefix `"clip[N]"` 供调试
   - **Indexing 约定**：`demux_by_clip_idx[ci]` parallel to `tl.clips[ci]`（video clip 的 slot 忽略；audio clip 的 slot 必须非 null）。这与 ComposeSink 现有 `demuxes` 参数的索引方式一致——future sink rewrite 直接复用。

2. **Scope 边界（明确不做）**：
   - **不**改任何 sink（H264AacSink / ComposeSink）——sub-scope (2) 下 cycle。
   - **不**翻 Exporter audio-only track gate——sub-scope (3)。
   - **不**做 2-track e2e determinism 测试——sub-scope (4)。
   - **不**支持 audio clip 的 `time_start != 0` 或 `source_start != 0`——AudioTrackFeed 继承其限制（解码器不做 seek；按 pull 顺序消费源）。phase-1 假设 audio clips 从 source 0 开始、按 time_start=0 播放。跨 clip boundary 的 audio concat（同 track 多 audio clip）也是未来工作——当前 builder 会开 N 个 feed 添加进 mixer，mixer 并行 pull 所有 feed，结果是**所有 clip 同时播放** 而非 sequential concat。下 cycle 在 sink wire-in 时处理这个问题（可能是 "one feed per track, seek-on-boundary" 或 "per-clip feed 但 mixer 按时间窗口启用"）。

3. **Tests** (`tests/test_audio_mixer.cpp`) —— +4 TEST_CASE / +1037 assertion（6/98764 → 10/99801）：
   - `no audio clips → NOT_FOUND`：timeline 只有 video → 返 ME_E_NOT_FOUND + err 含 "no audio clips"。
   - `size mismatch → INVALID_ARG`：`demux_by_clip_idx.size()` != `tl.clips.size()` → 返 ME_E_INVALID_ARG。
   - `1 audio clip + fixture demux builds pullable mixer`：手构 Timeline 带 1 audio track + 1 audio clip (`gain_db=-6`)，开 fixture demux 传进 builder → mixer with 1 track，pull 一帧 → silent（silent input × any gain = silent）+ frame_size=1024 + rate=48000。
   - `null demux for audio clip → INVALID_ARG`：timeline 有 audio clip 但 demux slot = null → 返 ME_E_INVALID_ARG + err 含 "null demux"。

**Alternatives considered.**

1. **Helper 直接返回 `AudioMixer` by value** —— 拒：AudioMixer 持有 `AVChannelLayout` 需 explicit uninit，`= default` move 不保证正确；unique_ptr 更清晰 + 让 caller 的 ownership 明确（可能放 class field / transfer to sink）。
2. **把 builder 做成 `AudioMixer::from_timeline(...)` 静态工厂** —— 拒：AudioMixer 不该知道 Timeline 类型（layering：audio/ 模块理论上独立于 timeline/ 模块）。free function 保持依赖方向 timeline → audio 或 audio+timeline → orchestrator。实际代码里 mixer.cpp 已经 include "timeline/timeline_impl.hpp"，但这只在 cpp 里；hpp 只前向声明 `me::Timeline`——依赖反向最小化。
3. **接受 `std::unordered_map<clip_id, DemuxContext>` 而非 `vector<shared_ptr<DemuxContext>>` by clip_idx** —— 拒：`std::unordered_map` 违反 VISION §5.3 determinism（iteration order 非确定）。现有 orchestrator pattern 都用 vector indexed by clip_idx；保持一致。
4. **Builder 同时接受 target_config 和默认（48k / FLTP / mono）** —— 保留 caller 指定 target。sink rewrite 时可能需要按 timeline 推断 target（如按所有 audio stream 的最大 sample_rate）——那是 sink 层决策，builder 不假设。
5. **Builder 把无效 gain_db（NaN / Inf）清理成 0 dB** —— 拒：current `parse_animated_static_number` 已在 loader 层拒绝非有限值；builder 信任上游。
6. **多 clip on same audio track 的 sequential concat 在本 cycle 处理** —— 拒：涉及 clip boundary 时的 feed 切换、source_start seek / AVFormatContext reopen —— scope 爆炸。当前行为（所有 audio clip 同时 play）对单-clip-per-track 的测试 case 不产生歧义；多 clip 跨 boundary 在 sink wire-in cycle 连同 decoder 切换机制一起解决。

**Scope 边界.** 本 cycle **不**做：
- Sink rewrite（sub-scope 1）。
- Exporter gate flip（sub-scope 2）。
- e2e determinism 测试（sub-scope 3）。
- Multi-clip-on-single-audio-track 正确 concat。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 27/27 suite 绿。
- `test_audio_mixer` 10 case / 99801 assertion（6/98764 → 10/99801；+4 case / +1037 assertion）。

**License impact.** 无。

**Registration.**
- `src/audio/mixer.hpp`：+ `build_audio_mixer_for_timeline` decl + forward decls for `me::Timeline` / `DemuxContext` / `CodecPool`。
- `src/audio/mixer.cpp`：+ impl + `#include "timeline/timeline_impl.hpp"`。
- `tests/test_audio_mixer.cpp`：+ 4 TEST_CASE。
- `docs/BACKLOG.md`：bullet `audio-mix-scheduler-wire` 文本 narrow —— AudioMixer builder 列入 prereq done，剩余 sub-scope (1) 的 sink wire-in / (2) Exporter gate / (3) e2e test 未动。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **未满足**——sink 未接。§M.1 不 tick。
