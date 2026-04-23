## 2026-04-23 — output-sink-interface (Milestone §M2-prep · Rubric §5.2)

**Context.** `Exporter::export_to` dispatched to `passthrough_mux` vs
`reencode_mux` via an inline `is_passthrough_spec() / is_h264_aac_spec()`
if-else. The pattern coupled:
- spec classification (what the caller asked for),
- per-codec parameter plumbing (video/audio bitrate, codec names),
- per-clip DemuxContext → segment plumbing,
- worker-thread capture-list bookkeeping,
all into a single ~150-line function. PAIN_POINTS 2026-04-22 flagged
this as "starts to pay on the second branch, compounds on the third".
Before adding a third spec (ProRes, HEVC, Opus-only audio, ...), invert
the control: classify once, dispatch via virtual call.

**Decision.**
- New `src/orchestrator/output_sink.{hpp,cpp}`:
  - `struct SinkCommon { out_path, container, on_ratio, cancel }` —
    params shared by every sink.
  - `struct ClipTimeRange { source_start, source_duration,
    time_offset }` — per-clip window; paired positionally with the
    demuxes passed to `process()`.
  - `class OutputSink { virtual me_status_t process(demuxes, err); }`
    — the one-method interface.
  - `make_output_sink(spec, common, ranges, err)` factory — the only
    site that classifies the spec.
  - Two concrete sinks, file-local (anonymous namespace):
    - `PassthroughSink` — zips `demuxes` with stored `ranges_` into a
      `PassthroughMuxOptions.segments` vector and calls
      `passthrough_mux`.
    - `H264AacSink` — single-clip-only; builds `ReencodeOptions` and
      calls `reencode_mux(*demuxes.front(), ...)`. The single-clip
      constraint lives in the factory (returns nullptr on multi-clip),
      so `process()` is guaranteed `demuxes.size() == 1`.
- `Exporter::export_to` — the before/after diff is the win:
  - Before: inline `is_passthrough / is_h264_aac` checks → capture a
    bundle of scalars (path, container, vcodec, acodec, vbr, abr,
    passthrough flag, cancel*) → worker thread reconstructs the
    `PassthroughMuxOptions` or `ReencodeOptions` via a second if-else
    inside the lambda.
  - After: factory call up front returns `std::unique_ptr<OutputSink>`
    (rejecting unsupported specs synchronously before the worker
    spawns), worker captures only the sink (via shared_ptr so the
    lambda is copyable — C++ lambda unique_ptr capture-by-move needs
    C++23 `[x = std::move(sink)]` + mutable, and gets tricky with
    std::function coercion, so shared_ptr is the pragmatic choice).
  - Line count dropped ~160 → ~130, with the codec-specific option-
    struct-building moved into the sink implementations where it
    belongs.
- ClipPlan now carries `ClipTimeRange` instead of three parallel
  `me_rational_t` fields, reducing the surface at the sink
  construction site.

**Alternatives considered.**
- **Template-based dispatch** (`Exporter<Sink>`): zero runtime cost
  but forces the spec classification to happen at compile time, which
  doesn't match the runtime-JSON-driven API. Rejected.
- **`std::variant<PassthroughOpts, ReencodeOpts>` handed to a free
  function**: avoids virtual calls but pushes the if-else into
  `std::visit` inside the worker — same structural problem, just
  with a different switch. Rejected.
- **Keep the spec classifiers in Exporter, let sinks just be POD
  options bundles**: partial win on plumbing, doesn't solve the
  worker-lambda capture bloat. Rejected — the sink owning its own
  params is the cleaner separation.
- **Make sinks construction-time immutable, no `SinkCommon` struct**:
  considered passing each param as a constructor arg but
  `PassthroughSink`'s ctor would be 4 args + ranges, and the factory
  repeats the same 4 args into `H264AacSink`. Bundling into
  `SinkCommon` is the DRY move. Kept.
- **Move `make_output_sink` into a registry like `task::registry`**:
  registry-based dispatch is the right shape when the set of sinks is
  extensible at runtime (host plug-ins, external codecs). Today the
  set is closed — 2 sinks, factory switch fits. Defer until a third
  sink reveals a real need for runtime registration.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build
  build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON` — both clean (`-Wall -Wextra -Wpedantic
  -Werror`).
- `ctest` Debug + Release: 6/6 suites pass. `test_determinism`
  (byte-equal passthrough rerender) is the critical one; this
  refactor changes the dispatch plumbing around the passthrough
  path, so passing there confirms the sink wrapper is semantic-
  preserving.
- `01_passthrough` → 2s MP4 (duration=2.000000, unchanged).
- Multi-clip concat (`/tmp/multi_clip.timeline.json`) → 4.02s MP4
  (duration=4.022993, unchanged).
- `05_reencode` → h264+aac MP4 (duration=2.020000, unchanged).

**License impact.** No dependency changes. All libav includes are
transitive via the already-linked passthrough_mux + reencode_mux.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory — untouched.
- **Orchestrator factory — new**: `make_output_sink(spec, common,
  ranges, err)` in `me::orchestrator`. Exporter is its only caller
  today.
- Exported C API — no new or removed symbols.
- CMake — `src/CMakeLists.txt` adds
  `orchestrator/output_sink.cpp`.
- JSON schema — untouched.
- Internal abstraction: `class OutputSink` (virtual `process`),
  concrete `PassthroughSink` + `H264AacSink` file-local to
  `output_sink.cpp`.
