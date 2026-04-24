## 2026-04-23 — audio-mix-sink-wire：接通 ComposeSink 的 AudioMixer 路径 + 翻 Exporter audio gate（Milestone §M2 · Rubric §5.1）

**Context.** Bullet `audio-mix-scheduler-wire` 的 sub-scope (1)(2) 合并交付：重构 ComposeSink 的 audio path 改走 AudioMixer；翻 Exporter 的 audio-only track UNSUPPORTED gate。之前的 cycle 已把 AudioTrackFeed / AudioMixer / build_audio_mixer_for_timeline builder 全部单独落地 + 单元测试过，本 cycle 是 end-to-end 接入。

Before-state grep evidence：
- `src/orchestrator/compose_sink.cpp:17-19`（pre-cycle）comment：`Audio is passthrough from demuxes[0]'s audio stream via the usual reencode audio path (no multi-track audio mixing yet — that's audio-mix-scheduler-wire)`。
- `src/orchestrator/compose_sink.cpp:494-517`（pre-cycle）audio flush 只 drain 一个永远空的 AVAudioFifo + flush encoder，没从任何地方 decode audio。产物的 audio stream 是 declared-but-empty。
- `src/orchestrator/exporter.cpp:69-75`（pre-cycle）硬拒 `tl.tracks[].kind == Audio`：`return ME_E_UNSUPPORTED "standalone audio tracks not yet implemented"`。

**Decision.**

1. **`src/orchestrator/compose_sink.cpp`** —— 两处接入：
   - `#include "audio/mixer.hpp"`。
   - 在 `setup_h264_aac_encoder_mux` 之后，如果 `tl_.tracks` 里有 `kind == Audio`：copy AAC encoder 的 target (`sample_rate`/`sample_fmt`/`ch_layout`/`frame_size`) 进 `AudioMixerConfig`，调 `build_audio_mixer_for_timeline(tl_, *pool_, demuxes, cfg, mixer, err)`。失败透传。`ME_E_NOT_FOUND`（timeline 声明 audio track 但没 audio clip）当警告处理——mixer 重置，fall through 到 legacy 空 audio flush。
   - Audio flush block 分支：
     - mixer 存在 → 循环 `pull_next_mixed_frame` 拉 1024-sample frame，stamp `pts = shared.next_audio_pts`（`next_audio_pts += nb_samples` 累加）、调 `detail::encode_audio_frame(mf, aenc, ofmt, out_aidx, err)`。
     - mixer null → 走旧 `drain_audio_fifo` 路径（空 FIFO + flush）。
     - 两条路径最后 `encode_audio_frame(nullptr, ...)` 通用 encoder flush。
   - Video compose loop 增加 `if (tl_.tracks[ti].kind == Audio) continue;` 跳过 audio track——否则 frame_source_at 对 audio track 返 SingleClip，loop 会尝试从 audio 的 clip_decoder 拉 video 帧（open_track_decoder 对 audio clip 同样会找 video stream，产生错乱）。

2. **`src/orchestrator/exporter.cpp`** —— 翻 audio-only gate：
   - 删除 `for (const auto& t : tl_->tracks) if (t.kind == Audio) return ME_E_UNSUPPORTED` 块。
   - 新增 `has_audio_tracks` 标志参与 `route_through_compose`：`is_multi_track || has_transitions || has_audio_tracks` → compose 路径接受 audio-only / 视频+音频 / transition / 多轨 四种都走 compose。

3. **`tests/test_compose_sink_e2e.cpp`** —— +1 TEST_CASE："ComposeSink e2e: video + audio track (mixer path) renders"：
   - 2-track timeline（`v0` kind=video + `a0` kind=audio），两 clip 都指向 with-audio fixture。
   - 调 `me_render_start` + `me_render_wait`，断 status=OK（或 videotoolbox 不可用时 skip）+ 文件大小 > 4096。实测 fixture 上产物 **315429 bytes** —— 比 video-only compose 明显多出的字节就是非空 AAC audio stream。

4. **`tests/test_timeline_schema.cpp`** —— 1 测试 title + 断言更新：
   - 原测试 "standalone audio track is rejected at render layer by Exporter" 断 err 含 "standalone audio tracks not yet implemented"。现在 Exporter gate 翻了；audio-only timeline 路由到 compose；compose factory 仍要求 h264+aac。测试 title 改为 "standalone audio track routes through compose; passthrough codec still rejected"，断言更新为 err 含 "compose path" + "h264"（即 factory 的通用 rejection 消息）。ME_E_UNSUPPORTED 返回码保持不变。

5. **`tests/CMakeLists.txt`** —— test_compose_sink_e2e 新增 `determinism_fixture_with_audio` 依赖 + `ME_TEST_FIXTURE_MP4_WITH_AUDIO` 宏 define，让新测试可以找到 with-audio fixture。

**Alternatives considered.**

1. **不改 compose frame loop，只改 audio flush block** —— 拒：`frame_source_at` 不知道 track 是 audio 还是 video，会对 audio track 返 SingleClip；video loop 跟着在 audio track 的 clip_decoder 上 pull video 帧，每帧都会产生一份 "stray video from audio track"。必须在 loop 顶加 track-kind skip。
2. **AudioMixer pull 的时机穿插在 video 帧 loop 里（interleaved encode）** —— 拒：video/audio 编码 + muxing 的顺序由 `av_interleaved_write_frame` 自己管 DTS 排序；sequential "video-all-then-audio-all" 依赖 interleaver 在末尾 reshuffle，这是 libav 常规 flow。穿插不 buy 任何 latency（ComposeSink 不是流式 preview，是 render-to-file）。
3. **Mixer 的 target 格式由 caller 显式传入，不从 encoder copy** —— 拒：encoder 的 sample_rate / sample_fmt / ch_layout / frame_size 已经是"下游想要的"；copy-from-encoder 保证 mixer 输出 encoder 直接接受、无二次 resample。caller 想改得显式重配 encoder。
4. **只接入 mixer，暂不翻 Exporter gate（audio-only timeline 仍 unsupported）** —— 拒：mixer 的真正价值是 audio-only + video+audio 都能走。gate 不翻只剩 video+audio 这一种情况覆盖，功能残缺。
5. **删 bullet 或保留到 e2e determinism test 落地** —— 选 narrow bullet 保留 sub-scope (3) 的 "mono 50Hz + mono 100Hz → stereo 48k" 合成-tone 测试。现有覆盖是 silent input，不足以证明 mixer 的实际 mix 行为（silence + silence = silence 是平凡对），真实 tone 的 sum + limit 是 M2 exit criterion 的 stronger evidence。
6. **合 audio mix + cross-dissolve 通过同一 test 做 e2e**—— 拒：两条独立功能，合测试会把 fail mode 耦合。各自一个 e2e test。

**Scope 边界.** 本 cycle **交付**：
- ComposeSink audio path 走 AudioMixer（timeline 有 audio track 时）。
- Exporter 接受 audio-only / video+audio timeline。
- e2e smoke test 证明 video + audio timeline 成功渲染非空 MP4（315KB）。

本 cycle **不做**：
- Sub-scope (3) 的 2-track determinism 测试（mono 50Hz + mono 100Hz → stereo 48k）。留作独立 cycle——真实 tone 合成 + 断言 mixed 样本数学正确性。
- Audio-only timeline（无 video 轨）的完整支持——ComposeSink 目前仍假设 tl.tracks[0] 是 video（`bottom_track_id = tl_.tracks[0].id`，`bottom_clip_indices` 必须非空），audio-only timeline 会在 factory 失败（"bottom track has no clips"）。属下一步工作。
- Multi-clip-on-single-audio-track 跨 clip boundary 的 sequential concat —— AudioTrackFeed + AudioMixer 当前让 N 个 audio clip 同时 play。只有 1-clip-per-audio-track timeline 行为正确。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 27/27 suite 绿。
- `test_compose_sink_e2e` 新增 1 case，产物 315KB（vs video-only compose 的 typical 200KB，差额是 audio stream）。
- `test_timeline_schema` audio-track rejection test title + 断言更新后通过。
- Legacy `test_compose_sink_e2e` 其他 5 个 case（video-only 2-track compose / opacity / translate / cross-dissolve / size-stability）全部 byte-identical（video-only 不触发 mixer path）。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_sink.cpp`：+~50 行（`#include "audio/mixer.hpp"` + mixer 构造块 + audio track skip in video loop + audio flush 分支）。
- `src/orchestrator/exporter.cpp`：-8 行（删旧 gate）+ 6 行（`has_audio_tracks` 接入 `route_through_compose`）。
- `tests/test_compose_sink_e2e.cpp`：+~80 行（新 TEST_CASE）。
- `tests/test_timeline_schema.cpp`：1 test title + 2 断言更新。
- `tests/CMakeLists.txt`：test_compose_sink_e2e 加 `determinism_fixture_with_audio` dep + `ME_TEST_FIXTURE_MP4_WITH_AUDIO` define。
- `docs/BACKLOG.md`：bullet `audio-mix-scheduler-wire` 文本 narrow——sub-scope (1)(2) 标 done，剩 (3) 2-track determinism test。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **仍未 tick**：
- src impl 非 stub ✓（AudioMixer + ComposeSink wire 都是实装）。
- CI 覆盖：test_audio_mixer 2-track silent fixture、test_compose_sink_e2e video+audio mixer path ✓（但都是 silent 输入，没非平凡 mix 数值）。
- 最近 feat commit ✓（本 cycle 及前 3 个 audio cycle）。
Evidence triple 的"CI coverage"项在 silent fixture 下只能证 pipeline 通，证不了 mix+limit 的数值正确性。保留未打勾，等 sub-scope (3) 的合成-tone 测试落地再 tick。§M.1 不 tick。
