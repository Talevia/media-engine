## 2026-04-23 — debt-io-mux-context-raii (Milestone §M1-debt · Rubric §5.2)

**Context.** Both `src/orchestrator/muxer_state.cpp` (passthrough) and
`src/orchestrator/reencode_pipeline.cpp` (h264/aac re-encode) were
hand-rolling the output AVFormatContext lifecycle:
`avformat_alloc_output_context2 → avio_open → write_header → ... →
write_trailer → avio_closep → free_context`. Each had a local `cleanup`
lambda duplicated across every error-return branch (~3–5 call sites per
TU). PAIN_POINTS logged this twice across 2026-04-22 and 2026-04-23,
including a sub-point about `av_interleaved_write_frame` zeroing the
packet after a successful write — the concat-snapshot bug that cost a
debug hour.

**Decision.**
- New `src/io/mux_context.{hpp,cpp}` introducing `me::io::MuxContext`:
  RAII holder for an output `AVFormatContext`. Mirrors
  `io::DemuxContext` on the input side.
  - Factory: `MuxContext::open(path, container, err*) →
    std::unique_ptr<MuxContext>`. Nullptr on failure (diagnostic in
    `*err`); no sentinel state.
  - Phase methods: `open_avio()`, `write_header()`, `write_trailer()`.
    Each is idempotent on success (double-call returns `ME_OK`);
    each tracks a bool so the destructor knows what to wind down.
  - Destructor: calls `avio_closep(&fmt->pb)` if `avio_opened_`;
    always calls `avformat_free_context(fmt)`. Never calls
    `write_trailer` — the decision to emit a trailer belongs to the
    caller (bailing mid-pipeline must not emit a bogus trailer).
  - `write_and_track(pkt, last_end_per_stream, err*)` — captures the
    "snapshot before `av_interleaved_write_frame` because it zeroes the
    packet" pattern in one place. Takes pkt ownership, unrefs
    regardless of outcome, advances per-stream monotonic end marker on
    success.
  - `AVFMT_NOFILE` formats (RTMP, pipe muxers) skip `avio_open` at the
    `open_avio` layer so calling code doesn't branch on the flag.
- `passthrough_mux` (`muxer_state.cpp`) — replaces the `ofmt +
  cleanup()` lambda pattern with `mux = MuxContext::open(...)`; every
  early-return no longer needs to call cleanup. The inline
  `av_interleaved_write_frame` + snapshot dance is replaced with a
  single `mux->write_and_track(pkt, last_end_out_tb, &write_err)` call.
- `reencode_mux` (`reencode_pipeline.cpp`) — same substitution on the
  output-AVFormatContext side. Encoder packet writes (video + audio)
  still go through `av_interleaved_write_frame` directly inside
  `encode_video_frame` / `encode_audio_frame` because those don't
  track per-stream monotonic end markers (encoder output PTS ordering
  is driven by the encoder, not by caller-side concat stitching);
  leaving them as-is keeps the diff minimal. They remain candidates
  for a later follow-up if a second concat-style feature path shows up.
- `src/CMakeLists.txt` — adds `io/mux_context.cpp` to the sources.

**Alternatives considered.**
- **Keep the `cleanup()` lambdas**: two files, ~8 call sites between
  them. Per-site maintenance cost is low when stable, but the sites
  *move* every time the pipeline grows (a new sws allocation, a new
  encoder option) because the lambda has to stay in scope. Too many
  invisible couplings between "where cleanup gets called" and "where
  new resources appear". Rejected.
- **Single big `io::Muxer` class** that also owns stream creation +
  per-stream encoders: covers more of the duplication, but mixes
  responsibilities (file lifecycle vs. codec wiring) and would need
  a templated `OutputSink` shape to stay polymorphic (see
  `output-sink-interface` backlog item). MuxContext deliberately
  stays narrow — just the file-open/header/trailer half — so the
  future OutputSink refactor can layer on top instead of rewriting.
  Rejected for this cycle; left for `output-sink-interface`.
- **Deleter-struct approach** (like the earlier
  `debt-ffmpeg-raii-shared-header` did for `AVCodecContext`): doesn't
  work cleanly because `AVFormatContext` output teardown is multi-step
  (`avio_closep` conditionally, then `avformat_free_context`), and
  requires tracking whether `avio_open` was ever called. A full class
  with state is the right shape; a deleter is not.
- **Also collapse the encoder-path `av_interleaved_write_frame` into
  `write_and_track`**: would consolidate further, but those call
  sites don't benefit from per-stream `last_end_out_tb` tracking
  (the encoder already produces monotonic PTS). Adding the helper
  there would just mean ignoring its main value. Left as-is.
- **RAII a pair of `std::unique_ptr<AVFormatContext, …>` guards**:
  forces you to resolve the `avio_open` branch with a second guard,
  which partially motivated PAIN_POINTS in the first place. A class
  expressing the three-phase state machine is clearer than stacking
  unique_ptr deleters.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build
  build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON` — both clean.
- `ctest` Debug + Release: 6/6 suites pass. `test_determinism` is the
  critical one — it renders the same passthrough timeline twice and
  byte-compares. Had the refactor broken packet-write ordering or
  ts-rebasing, this would have caught it.
- `01_passthrough` regression (2s MP4) — produces valid output;
  ffprobe-reported duration matches pre-refactor (2.000000s).
- `01_passthrough` on multi-clip fixture (`/tmp/multi_clip.timeline.json`
  with two 2s clips) — produces 4.022993s output (same as
  multi-clip-single-track cycle); confirms `write_and_track`'s
  snapshot logic still advances `last_end_out_tb` correctly across
  the seg 0 → seg 1 boundary that originally motivated the
  PAIN_POINTS entry.
- `05_reencode` — produces valid h264+aac MP4.

**License impact.** No dependency changes. MuxContext uses the same
libavformat functions that `DemuxContext` already binds.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched. (MuxContext is an I/O
  resource, not a graph Node — same positioning as `DemuxContext`.)
- Resource factory — untouched.
- Orchestrator factory — untouched; `passthrough_mux` and
  `reencode_mux` internals changed but the function signatures that
  Exporter calls are stable.
- Exported C API — no new or removed symbols.
- CMake — `src/CMakeLists.txt` adds `io/mux_context.cpp`.
- JSON schema — untouched.
- New internal source: `src/io/mux_context.{hpp,cpp}`.
