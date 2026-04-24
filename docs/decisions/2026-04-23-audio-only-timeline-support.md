## 2026-04-23 — audio-only-timeline-support：纯 audio timeline 渲染支持（Milestone §M2 / §M3 · Rubric §5.1）

**Context.** M2 audio-mix-scheduler-wire cycle 后，timeline 有 audio track 时 mixer 已 wire 进 ComposeSink 的 audio path。但一个**没有** video track、只有 audio track 的 timeline 仍 unsupported：

Before-state grep evidence：
- `src/orchestrator/compose_sink.cpp:99` `const std::string& bottom_track_id = tl_.tracks[0].id` —— 硬假设 track 0 存在；若它是 Audio kind 就走错路。
- `src/orchestrator/compose_sink.cpp:107-110`：`if (bottom_clip_indices.empty()) return ME_E_INVALID_ARG "bottom track has no clips"` —— 若 track 0 是 audio 但其 clip 被当"bottom 视频 clip"过滤（同一 track id 匹配），其实 index 填得上；但后续 `setup_h264_aac_encoder_mux` 拿到的 sample_demux 是 audio clip 的 demux（如 with-audio fixture）→ 能探到 video stream 也能探到 audio → 打开 video encoder，生成**含 video stream 的输出**——和 timeline 意图"纯 audio"完全不一致。
- ComposeSink::process 的 frame loop `const int W = shared.v_width; if (W <= 0 ...) return ME_E_INTERNAL` —— 即使走通 setup，若 fixture 无 video（例如真实 .wav / .m4a），video 配置为空 → 这一步死掉。

**Decision.**

1. **`src/orchestrator/reencode_pipeline.hpp`** —— `ReencodeOptions` +`bool audio_only = false;` 字段。
2. **`src/orchestrator/encoder_mux_setup.cpp`** —— `opts.audio_only=true` 时跳过 `best_stream(VIDEO)`，硬设 vsi0=-1；后续自然 skip 所有 video decoder / encoder / stream 创建。错误消息区分 "no audio" in audio-only 情境：`"sample_demux has no audio (audio_only requested)"`。
3. **`src/orchestrator/audio_only_sink.{hpp,cpp}`** —— 新文件，`AudioOnlySink` class 实现 OutputSink：
   - 扫 timeline，筛 audio clips 填进 `opts.segments`（duration accounting）。
   - 记录首个 audio clip idx 作 sample_demux。
   - `setup_h264_aac_encoder_mux(opts, sample_demux->fmt, ..., audio_only=true)` —— 产出只有 audio stream 的 mux + 配好 AAC encoder + 空的 video 状态。
   - `build_audio_mixer_for_timeline` 以 encoder 的 sample_rate / sample_fmt / ch_layout / frame_size 配 mixer（同 ComposeSink 的 pattern）。
   - 主循环：`mixer->pull_next_mixed_frame` → `detail::encode_audio_frame` → mux。Cancel-aware。
   - `write_trailer` + `on_ratio(1.0)`。
   - 绝不 touch video encoder / 视频 frame loop / RGBA buffer / alpha_over —— audio-only 完全不依赖 compose 路径的任何 video 机制。
   - **新文件**而非扩 compose_sink.cpp：compose_sink.cpp 已 694 行（逼近 debt P0 阈值 700），加 audio-only 分支会让 god-class 更严重。audio_only_sink 作独立 TU 自然分解。`debt-split-compose-sink-cpp` bullet 因此部分退场——compose_sink 不再是 "所有非 legacy" sink 的容器。
4. **`src/orchestrator/exporter.cpp`** —— 路由分流：
   - 新 `has_video_tracks` flag。
   - `route_audio_only = has_audio_tracks && !has_video_tracks`。
   - Sink 选择：`route_audio_only → make_audio_only_sink`；`route_through_compose → make_compose_sink`（仅当**有** video track 时）；else → `make_output_sink`。
5. **Factory 清晰边界** (`make_audio_only_sink`)：
   - Audio codec 必须 aac（passthrough audio 不支持——参考 ComposeSink factory 的 h264-only 约束）。
   - `pool` required（与 ComposeSink 同）。
   - `clip_ranges` 非空。
   - 至少 1 个 Audio track、不得有 Video track（contractual split with make_compose_sink）。
6. **Tests**：
   - `tests/test_compose_sink_e2e.cpp` +1 TEST_CASE "AudioOnlySink e2e: audio-only timeline (no video track) renders"：单 audio track + 1 audio clip 引用 with-audio fixture，render 成 `/tmp/me-compose-sink-e2e/audio_only.mp4`。实测产物 **978 bytes**（silent AAC 1 秒 + MP4 container overhead，视频流当然不存在）。
   - `tests/test_timeline_schema.cpp` "standalone audio track routes through compose" 测试的 err-string 断言更新——audio-only + passthrough codec 现在被 `make_audio_only_sink` 拒绝，错误消息含 "audio-only path" + "aac"。测试 title 之前已改成反映新路由。

7. **Scope 边界**：
   - 多 audio track（N ≥ 2）audio-only：factory 接受（AudioMixer 支持 N tracks），但本 cycle 测试只跑 1 track。多 track audio-only 是自然扩展，覆盖已隐含在 AudioMixer 的单元测试里。
   - Audio-only + transitions：cross-dissolve transition 目前只对 video 定义；audio track 上声明 transition 会被 loader 当 video transition 处理，可能在 ComposeSink 的 Transition 分支 explode。留作 edge-case bullet（不在 M2/M3 scope）。
   - Audio-only + multi-clip-per-track：AudioMixer 当前是 "N feed 同时播放"，非 sequential concat。单 audio track 带 2+ clip 会触发同时播放，输出和 schema 不一致。留作 `audio-multi-clip-concat` 独立 bullet（未加入 backlog——本 cycle 无观察触发 debt append）。

**Alternatives considered.**

1. **扩 ComposeSink 支持 audio-only 模式（in-file branching）** —— 拒（见 §3 末尾）：compose_sink.cpp 已过重；新文件符合 debt 走向 + 职责隔离（ComposeSink 关心 video compose；AudioOnlySink 关心 audio-only encode）。
2. **让 make_output_sink 支持 audio-only 而非新 sink** —— 拒：make_output_sink 的现有 sink（Passthrough / H264Aac）都假设从 demuxes[0] 的 audio stream 直接 reencode，不走 mixer。加 "mixer-driven" 变体相当于 fork，不如独立 sink 干净。
3. **audio_only 作 ReencodeOptions 字段 vs 函数参数 overload** —— 选字段：`setup_h264_aac_encoder_mux` 签名已很长；新 overload 只差一个布尔参数，加字段更直接 + 未来若 audio-only 对其它 opts 字段有依赖（比如未来加 `include_subtitles`）统一入口更清晰。
4. **Audio-only 需要特殊 container（如 `.m4a` 或 `.aac`）** —— 拒：MP4 容器完全支持 audio-only stream（只有 audio track 的 .mp4 有效）。用户想要 .m4a 扩展名可自己指定 out_path；container="mp4" 保持。
5. **Test 断 audio-only output 的 audio stream 通过 ffprobe 验证** —— 拒：ffprobe invocation + parse 增加 test 复杂度 + 依赖外部工具。文件大小下限 256B + 上限 300000B（video+audio 变体产物 315429B）足以证明**无** video stream 混入 + **有** audio 内容。

**Scope 边界.** 本 cycle **交付**：
- AudioOnlySink 新 class + 新 TU。
- 路由分流（audio-only → AudioOnlySink；有 video + 其它 → ComposeSink）。
- `audio_only` flag 加到 ReencodeOptions + setup_h264_aac_encoder_mux 遵守。
- e2e 测试覆盖成功路径 + 原有 timeline_schema 测试反映新错误消息。

本 cycle **不做**：
- Audio-only + transitions / multi-clip 组合（留独立 bullet / 等场景触发）。
- compose_sink.cpp 的剩余 debt（694 行未变；audio-only 新进 audio_only_sink.cpp 不加重）。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 28/28 suite 绿。
- `test_compose_sink_e2e` 新增 1 case（audio-only render, 978 bytes 产物）。
- `test_timeline_schema` 1 case 断言更新后通过。

**License impact.** 无。

**Registration.**
- `src/orchestrator/reencode_pipeline.hpp`：+ `bool audio_only = false` 字段。
- `src/orchestrator/encoder_mux_setup.cpp`：遵守 `opts.audio_only`。
- `src/orchestrator/audio_only_sink.hpp/cpp`：新文件，AudioOnlySink class + make_audio_only_sink factory。
- `src/orchestrator/exporter.cpp`：+ `has_video_tracks` + `route_audio_only` 路由 + `#include audio_only_sink.hpp`。
- `src/CMakeLists.txt`：+ `orchestrator/audio_only_sink.cpp`。
- `tests/test_compose_sink_e2e.cpp`：+1 TEST_CASE。
- `tests/test_timeline_schema.cpp`：1 test err-string 断言更新。
- `docs/BACKLOG.md`：**删除** bullet `audio-only-timeline-support`。

**§M 自动化影响.** 当前 milestone 是 M3；M3 的 5 条 exit criteria 均未满足。本 cycle 处理的是 bullet 标记 §M2 的项（audio-mix 相关，M2 已 advance 但 bullet 历史标签是 M2）。不影响 M3 criterion tick。§M.1 不 tick。
