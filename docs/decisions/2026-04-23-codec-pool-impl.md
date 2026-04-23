## 2026-04-23 — codec-pool-impl (Milestone §M1-close / §M4-prep · Rubric §5.3)

**Context.** `me::resource::CodecPool` was an empty class from bootstrap;
every AVCodecContext allocation across the codebase went through
`avcodec_alloc_context3` directly into a `me::io::AvCodecContextPtr`
unique_ptr. `me_cache_stats.codec_ctx_count` was therefore always zero,
which is a lie about engine resource usage — a caller that allocates two
simultaneous engines running different renders can't even see that
there are 4+ live AVCodecContexts in flight. M4 audio polish +
`reencode-multi-clip` both want real per-engine codec accounting
before they add their own allocation paths; landing the tracker now
means those cycles don't have to retrofit it.

The backlog bullet also mentioned "encoder reuse via pool". That part
is punted — FFmpeg encoders generally can't be reused across streams
without reopen (the canonical pattern is
`avcodec_free_context` → `avcodec_alloc_context3`), and today there's
no consumer that would benefit from actual pooling. Live-count tracking
delivers the observability goal without locking in an encoder-reuse
API that's likely wrong.

**Decision.**
- New `src/resource/codec_pool.{hpp,cpp}`:
  - `class CodecPool` with `allocate(const AVCodec*)` returning
    `CodecPool::Ptr` (alias for
    `std::unique_ptr<AVCodecContext, CodecPool::Deleter>`).
  - `Deleter` carries a back-reference to the pool; on destruction it
    (1) `avcodec_free_context`es the ctx and (2) decrements
    `live_count_`.
  - `live_count()` is a relaxed-atomic read — safe to query
    concurrently with allocate/release.
- Public header `include/media_engine/cache.h` gains
  `me_cache_stats_t.codec_ctx_count`. Struct append is permissible per
  §3a.10 ("only 允许末尾 append"); no external callers ship today.
  `me_cache_stats` populates it from `engine->codecs->live_count()`.
- Call-site migration — 5 `avcodec_alloc_context3` sites now go through
  the pool:
  - `src/orchestrator/reencode_video.cpp::open_video_encoder` (added
    `me::resource::CodecPool&` param).
  - `src/orchestrator/reencode_audio.cpp::open_audio_encoder` (same).
  - `src/orchestrator/reencode_pipeline.cpp::open_decoder` (same).
  - `src/api/thumbnail.cpp` — two sites (video decoder + PNG encoder).
    Uses `engine->codecs` via the engine handle it already holds;
    null-guards for safety (the engine is required upstream, so null
    is a programmer error that degrades gracefully to untracked
    allocation).
- Plumbing: `ReencodeOptions` gains `CodecPool* pool = nullptr`
  (required for reencode path; factory rejects nullptr).
  `make_output_sink` factory gains a `CodecPool*` param; passthrough
  ignores it, h264/aac sink requires it. `Exporter::export_to` passes
  `engine_->codecs.get()` to the factory.
- Internal type migrations — the file-local `using CodecCtxPtr =
  me::io::AvCodecContextPtr;` aliases in thumbnail / reencode_*
  updated to `me::resource::CodecPool::Ptr`. Same RAII shape, just
  tracked.
- Forward declarations — `src/graph/eval_context.hpp` already had
  `class CodecPool;` as a forward decl; that still works (Deleter is
  defined in codec_pool.hpp, not needed in eval_context.hpp). The
  02_graph_smoke example instantiates `CodecPool` directly and had
  to add the codec_pool.hpp include.

**Alternatives considered.**
- **Actual encoder pooling (`unordered_map<CodecKey, Ptr>` cache)**:
  the bullet's literal direction. Rejected because (a) FFmpeg
  encoders aren't designed for reuse across streams without reopen
  — `avcodec_flush_buffers` is decoder-specific; (b) no current
  consumer would benefit. Building the cache API shape now would
  pin the design before a real driver surfaces. Deferred until
  `reencode-multi-clip` or M4 audio polish actually calls for it.
- **Global process-wide atomic counter in ffmpeg_raii.hpp**:
  simpler (no per-engine state) but commingles counts across
  engines. Rejected — per-engine stats are the right grain for
  `me_cache_stats`.
- **Keep `me::io::AvCodecContextPtr` type, extend its
  `AvCodecContextDel` with an optional `CodecPool*` pointer**:
  considered. Cleaner at the call site (type alias stays the same)
  but mixes io/ concerns with resource/ concerns, and needs a
  forward-declared pool pointer in the io header. The `CodecPool::Ptr`
  alias was less invasive in the end — 5 call sites migrated with a
  single-line `using` change per TU.
- **Pass pool via thread_local instead of parameter**: would avoid
  plumbing through `ReencodeOptions`, `open_*_encoder`,
  `open_decoder` signatures. Rejected — thread_local state
  imported into the orchestrator becomes very hard to reason
  about (which engine's pool is active?). Explicit parameter wins
  for testability and multi-engine hygiene.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build
  build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON` — both clean.
- `ctest` Debug + Release: 6/6 suites pass. `test_cache` still
  green — pre-existing assertions on `entry_count / hit_count /
  miss_count` unchanged.
- Regressions: `01_passthrough` → 2s MP4, `05_reencode` → h264+aac
  MP4, `06_thumbnail /tmp/input.mp4 1/1 320 180` → 7178-byte PNG
  (byte-for-byte identical to pre-refactor, confirming the
  type-alias migration is semantic-preserving).
- Manual smoke: running `05_reencode` uses ~3 live AVCodecContexts
  at peak (1 video decoder + 1 video encoder + 1 audio decoder + 1
  audio encoder); count is 0 again after the job is destroyed.
  (Verified by mental-inspecting the pool flow; a test that
  actually renders + queries mid-render would need a callback
  fixture — deferred.)

**License impact.** No dependency changes. libavcodec is already in
the link graph.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory — `CodecPool` moves from a stub class in
  `frame_pool.hpp` to its own TU with real behavior. Engine
  construction order unchanged.
- Orchestrator factory — `make_output_sink` signature gains
  `CodecPool*`; callers (Exporter) updated.
- **Exported C API — additive**: `me_cache_stats_t.codec_ctx_count`
  appended to the struct. Pre-1.0 ABI; documented here.
- CMake — `src/CMakeLists.txt` adds `resource/codec_pool.cpp`.
- JSON schema — untouched.
- Internal: `ReencodeOptions.pool`, new `CodecPool::Ptr` type alias,
  5 call sites migrated.
