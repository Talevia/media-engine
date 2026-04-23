## 2026-04-23 — debt-split-reencode-pipeline (Milestone §M1-debt · Rubric §5.2)

**Context.** `scan-debt.sh` §1 long-files signal flagged
`src/orchestrator/reencode_pipeline.cpp` at 615 lines — in the
400-to-700 P1-default band. The file had accumulated three concerns
in one TU: (a) video encoder setup + per-frame encode, (b) audio
encoder setup + per-frame encode, (c) the read→decode→dispatch→mux
orchestration loop plus the AAC-frame-size FIFO drain. Splitting the
(a) and (b) helpers out lets the pipeline orchestration read as one
concern and gives future per-codec changes a narrower footprint.

**Decision.**
- **`src/orchestrator/reencode_video.{hpp,cpp}`** — exports two
  functions in `me::orchestrator::detail`:
  - `open_video_encoder(dec, stream_tb, bitrate, global_header,
    out_enc, out_target_pix, err)` — `h264_videotoolbox` setup + NV12
    target pix_fmt resolution.
  - `encode_video_frame(in_frame, enc, sws, scratch_nv12, ofmt,
    out_stream_idx, in_stream_tb, err)` — send_frame →
    receive_packet → interleaved_write_frame loop, with optional
    sws_scale into NV12 staging.
  - 118 lines total.
- **`src/orchestrator/reencode_audio.{hpp,cpp}`** — exports:
  - `open_audio_encoder(dec, bitrate, global_header, out_enc, err)` —
    libavcodec built-in AAC, sample-rate clamp to MPEG-4 AAC grid,
    FLTP sample format, channel-layout inherit-or-default.
  - `encode_audio_frame(in_frame, enc, ofmt, out_stream_idx, err)` —
    send_frame → receive_packet → write loop for AAC output.
  - 108 lines total.
- **`src/orchestrator/reencode_pipeline.cpp`** stays as orchestration:
  - `open_decoder` (shared by both media types, stays here).
  - `drain_audio_fifo` lambda — captures `afifo / aenc / ofmt /
    out_aidx / next_audio_pts / err`. Extracting to a free function
    would need 6 parameters or a state-holder class; no second
    consumer, so it stays in the orchestrator TU where its captures
    are natural.
  - `reencode_mux` top-level flow.
  - Drops from 615 → 432 lines (still over 400 but under the 700 hard
    cap; further shrink would need the `AudioPipeline` state-class
    refactor, which has no second consumer to justify it yet).
- Both new TUs use short file-local aliases `CodecCtxPtr` /
  `PacketPtr` over `me::io::AvCodecContextPtr` /
  `me::io::AvPacketPtr` from `io/ffmpeg_raii.hpp`, matching the style
  established by `reencode_pipeline.cpp` and `thumbnail.cpp`.
- `reencode_pipeline.cpp` brings the detail-namespace helpers into
  scope via `using detail::open_video_encoder;` etc. so the top-level
  flow reads the same as before the split.
- `av_err_str` is duplicated into each new TU (3-line helper). A
  shared `src/io/av_err.hpp` would be nicer but adds a scope creep
  that's out of bounds for this cycle — noted for a later
  consolidation, tracked implicitly by `scan-debt.sh` §5 and the
  next debt cycle.

**Alternatives considered.**
- **Full `VideoEncoder` / `AudioEncoder` classes** that own encoder
  context + scratch frames + sws/swr state: cleanest on paper, but a
  large refactor that reshapes the orchestration flow too. The
  current split lets `open_*` and `encode_*` stay stateless free
  functions so the orchestrator TU stays the state owner. When
  `reencode-multi-clip` lands it will naturally want to reuse
  encoder state across clips — that's the trigger to promote these
  into classes. Rejected for this cycle; deferred.
- **Put `drain_audio_fifo` in `reencode_audio.cpp`**: it's audio-
  specific, after all. But extracting it means turning the lambda
  into a struct or passing 6 unrelated parameters; the orchestrator
  TU is where the FIFO lives anyway. Keeping it there.
- **Extract only `encode_video_frame` and leave the encoder setup in
  the orchestrator**: less useful — the setup + encode pairs naturally
  live together, and splitting them out mid-function would make the
  orchestrator diff ugly. All-or-nothing per media type.
- **Rename `reencode_pipeline.cpp` → `reencode.cpp` now that it's
  smaller**: the filename vs. class-name convention is already mixed
  (`muxer_state.cpp` holds `passthrough_mux`, not a `MuxerState`
  class). Not worth the rename churn.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build
  build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON` — both clean (`-Wall -Wextra -Wpedantic
  -Werror`).
- `ctest` Debug + Release: 6/6 suites pass, including
  `test_determinism` which exercises passthrough byte-equality.
- `05_reencode` regression on the 2s sample — produces valid
  h264+aac MP4, duration 2.02s (AAC priming tail as expected).
- `01_passthrough` + multi-clip concat — both still produce the
  same outputs as prior cycles. This refactor only moves
  encoder-side code, so passthrough behavior is untouched, but
  the shared build graph ensures a compile regression would be
  caught by the mux-side suites too.

**License impact.** No dependency changes. All headers consume the
same libavcodec/libavformat/libswscale surface that the monolithic
file did.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory / orchestrator factory — untouched at the
  factory / public interface level; `reencode_mux` signature
  unchanged.
- Exported C API — no new or removed symbols.
- CMake — `src/CMakeLists.txt` adds `orchestrator/reencode_video.cpp`
  and `orchestrator/reencode_audio.cpp` to the sources list.
- JSON schema — untouched.
- New internal sources: `reencode_video.{hpp,cpp}`,
  `reencode_audio.{hpp,cpp}`.
