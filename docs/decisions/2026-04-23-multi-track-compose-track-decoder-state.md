## 2026-04-23 — multi-track-compose-track-decoder-state（scope-A of actual-composite：per-track 状态包 + open helper）（Milestone §M2 · Rubric §5.2）

**Context.** Real compose loop 需要 per-track 状态（每 track 一个 demux + video decoder + 复用的 scratch pkt/frame）。上 cycle 做的 `pull_next_video_frame` 是 free function 接受 raw pointer。compose loop 会有 N 个 track，手动 open 每个 decoder + 管理 scratch buffers 会让 process() 函数变得冗长（N × ~30 LOC 的 boilerplate）。把这个组合抽成数据结构 + open helper，compose loop 变得更 focused。

Before-state grep evidence：

- `grep -rn 'TrackDecoderState\|open_track_decoder' src/` 返回空。
- `src/orchestrator/frame_puller.hpp` 是上 cycle 新加，只有 `pull_next_video_frame` 一个函数。

**Decision.** 扩展 `src/orchestrator/frame_puller.hpp/cpp`（而不新建第三个 TU——`frame_puller` 已经是 "per-track video decode 原语" 的合适家）：

1. **`struct TrackDecoderState`**：
   ```cpp
   struct TrackDecoderState {
       std::shared_ptr<me::io::DemuxContext> demux;
       int                                    video_stream_idx = -1;
       me::resource::CodecPool::Ptr           dec;
       me::io::AvPacketPtr                    pkt_scratch;
       me::io::AvFramePtr                     frame_scratch;
   };
   ```
   - `video_stream_idx < 0` → demux 无 video stream（未来 audio-only track 用，compose loop 跳过该 track 的 decode）。
   - 所有字段 RAII：dec 是 `CodecPool::Ptr`（unique_ptr），pkt/frame 是 `AvPacketPtr`/`AvFramePtr`（unique_ptr with custom deleter），demux 是 shared_ptr。move-only 自动。

2. **`me_status_t open_track_decoder(demux, pool, out, err)`**：
   - 接 shared_ptr<DemuxContext> + CodecPool&，populate `out`。
   - Null demux → `ME_E_INVALID_ARG`。
   - `av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, ...)` 找 video stream；负值保留（no video，下游 compose loop 跳过该 track）。
   - Allocate scratch packet + frame 永远（即使 no video，统一接口）。
   - 若有 video stream → `detail::open_decoder(pool, stream, out.dec, err)` 开解码器；失败 reset out 返回 status。
   - 成功 → out.demux = 移动进来的 demux，out.video_stream_idx = vsi。

3. **Tests**（扩展 `tests/test_frame_puller.cpp`，+2 TEST_CASE = 6 total / 54 assertion，之前 4/42）：
   - `open_track_decoder` 从 fixture demux 开成功：state.demux / video_stream_idx / dec / pkt_scratch / frame_scratch 都非 null。紧接着用返回的 state 调 `pull_next_video_frame` 拉一帧验证 wiring 正确，CHECK width=640 height=480。
   - `open_track_decoder(nullptr, ...)` → `ME_E_INVALID_ARG`。

**Scope 留给 follow-up `multi-track-compose-actual-composite`**：
- ComposeSink::process body 现在可以简化成：
  1. Build ReencodeOptions 从 common_ + spec 组合。
  2. Call `setup_h264_aac_encoder_mux(opts, demuxes[0]->fmt, mux, venc, aenc, shared, err)`。
  3. mux->open_avio + write_header。
  4. `std::vector<TrackDecoderState> track_decoders;` for each track → `open_track_decoder(demuxes[bottom_clip_of_track], pool, td, err)` → push_back。
  5. Per output frame T at tl.frame_rate：
     - `active_clips_at(tl_, T)` → TrackActives。
     - For each TA in order：`pull_next_video_frame(td.demux->fmt, td.video_stream_idx, td.dec.get(), td.pkt_scratch.get(), td.frame_scratch.get(), err)` → 若 NOT_FOUND track EOF 跳；若 OK → `frame_to_rgba8` → `alpha_over` into dst_rgba。
     - `rgba8_to_frame(dst_rgba, target_yuv)` → `encode_video_frame(target_yuv, ...)`。
  6. Audio：phase-1 走 `demuxes[0]` audio stream passthrough（复用 reencode_segment 的 audio path）。
  7. Flush encoders + write_trailer。

**Alternatives considered.**

1. **另起新 TU `track_decoder_state.{hpp,cpp}`** —— 拒：和 `pull_next_video_frame` 是同一层抽象（per-track video decode primitives），合到 `frame_puller.{hpp,cpp}` 里避免文件碎片化。
2. **Class 而非 struct** —— 拒：只有 data members + 一个 factory；struct + free function 足够清晰；成员全 RAII，不需要封装方法。
3. **move ctor / move assign 显式声明** —— 拒：`std::shared_ptr` / `std::unique_ptr` 成员都是 move-friendly，编译器默认 move 正确。
4. **给 `video_stream_idx < 0` 错的 demux 返回 `ME_E_UNSUPPORTED`** —— 拒：audio-only track 是合法 IR（`audio-mix-two-track` cycle 加的 TrackKind::Audio），compose loop 应该跳过而不是 factory 拒绝。`video_stream_idx = -1` 是正确 sentinel。
5. **把 `pool` 储存到 struct 里**（而非只在 open 时用） —— 拒：pool 生命周期由 engine 管；CodecPool::Ptr 本身 remember 了 pool（unique_ptr with pool-specific deleter）；struct 不需要 back-reference。
6. **把 `open_track_decoder` 改成 `TrackDecoderState` 的 static factory method** —— 拒：free function + out-param 模式和 `setup_h264_aac_encoder_mux` 一致；库内部 convention。
7. **audio decoder 也一起 open**—— 拒：audio 走 `demuxes[0]` passthrough（phase-1），不需要 per-track audio decoder。future `audio-mix-scheduler-wire` cycle 再扩（可能加独立 `TrackAudioDecoderState` struct）。

业界共识来源：Gstreamer 的 `GstDecodeBin` per-stream element 聚合、FFmpeg transcode.c 的 `InputStream` struct（stream_index + ist_index + dec_ctx + frame + pkt）——都是把"per-source decode state" 作为数据结构。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 23/23 suite 绿。
- `build/tests/test_frame_puller` 6 case / 54 assertion（前 4 cases / 42 assertion 保持 + 2 new cases / +12 assertion）。
- 其他 22 suite 全绿。

**License impact.** 无新 dep。

**Registration.**
- `src/orchestrator/frame_puller.{hpp,cpp}`：`struct TrackDecoderState` + `open_track_decoder` 声明和实装。frame_puller.hpp include 新增 `io/ffmpeg_raii.hpp` + `resource/codec_pool.hpp` + `<memory>`。frame_puller.cpp include 新增 `io/demux_context.hpp` + `orchestrator/reencode_segment.hpp`（for `detail::open_decoder`）。
- `tests/test_frame_puller.cpp`：+2 TEST_CASE，include `io/demux_context.hpp` + `resource/codec_pool.hpp`。
- **无** `src/CMakeLists.txt` / `tests/CMakeLists.txt` 改动——TU 数量不变。
- **无** `docs/BACKLOG.md` 改动——`multi-track-compose-actual-composite` bullet 文本已经预期 per-track decoder 分别 open，本 cycle 的抽象是实现细节的 scope-A 切片，bullet direction 不需要更新。

**§M 自动化影响.** 本 cycle Rubric §5.2（developer experience），不对应 exit criterion。§M.1 不 tick。
