## 2026-04-23 — content-hash-asset (Milestone §M1 · Rubric §5.1 + §5.4)

**Context.** `docs/TIMELINE_SCHEMA.md` already documents
`assets[].contentHash: "sha256:<hex>"` as a strongly-recommended field
for cache-key short-circuit. The loader silently ignored it, and the
engine had nowhere to store a computed hash — so every future cache
lookup would have to re-hash the asset on every run. VISION §3.3 hinges
on stable content-hash keys; M6 frame cache and M4 CodecPool both need
this foundation before they can land.

**Decision.**
- **`src/resource/content_hash.{hpp,cpp}`** — thin libavutil wrapper:
  - `sha256_hex(const uint8_t*, size_t)` — in-memory path, returns 64-char
    lowercase hex.
  - `sha256_hex_streaming(path_or_uri, err*)` — `fopen` + 64 KiB read loop
    into `av_sha_update`, streaming so a 20 GB asset doesn't touch heap
    beyond the buffer. Accepts `file://` URIs; strips the scheme the same
    way `probe.cpp` and `thumbnail.cpp` do.
  - Uses libavutil's AVSHA (already in link graph, LGPL). Zero new
    dependencies — OpenSSL / libsodium considered and rejected because
    introducing a hash-only dep for ~40 lines of wrapper isn't worth the
    ARCHITECTURE.md whitelist churn.
- **`src/resource/asset_hash_cache.{hpp,cpp}`** — per-engine
  `URI → hex sha256` map, thread-safe:
  - `seed(uri, hex)` — trusted JSON hash goes in without recompute; empty
    hex is a no-op (so the timeline loader can call unconditionally).
  - `get_or_compute(uri, err*)` — cached → fast path; otherwise streams
    through sha256 outside the mutex (a 1 GB file shouldn't block other
    engine threads' lookups), then `try_emplace` under the lock (if two
    threads race, both get the same hex — file contents are immutable).
  - `peek(uri)` — cache lookup without compute, for diagnostics / tests.
- **Engine ownership** (`src/core/engine_impl.hpp`) — add
  `std::unique_ptr<me::resource::AssetHashCache> asset_hashes;` alongside
  `frames` / `codecs`. Constructed in `me_engine_create`.
- **Loader** (`src/timeline/timeline_loader.cpp`) — the local
  `asset_map: id → {uri, hash_hex}` now parses `contentHash`:
  - Require the `"sha256:"` prefix; any other algorithm (`"md5:"`,
    `"blake3:"`, etc.) → `ME_E_UNSUPPORTED` with a clear diagnostic.
  - Validate 64-char hex (positions + alphabet). Reject non-hex with
    `ME_E_PARSE`.
  - Normalize to lowercase (cache-key stability).
  - Propagated into `me::Clip::content_hash` (new field). Empty string
    remains the "unknown, compute lazily" sentinel.
- **`me_timeline_load_json`** (`src/api/timeline.cpp`) — after loader
  succeeds, iterate `out->tl.clips` and call `engine->asset_hashes->seed`
  for each clip with a non-empty hash. Clips that share an asset URI
  seed the same entry idempotently.
- **Tests** (`tests/test_content_hash.cpp`, 8 cases / 22 assertions):
  - NIST vectors: `sha256("abc") = ba78...`, `sha256("") = e3b0c4...`.
  - Streaming a temp file matches in-memory sha of its bytes.
  - `file://` scheme prefix handled (matches raw path result).
  - Missing file → empty string + `err` populated.
  - `AssetHashCache::get_or_compute` caches (delete file after first
    call; second call still returns the cached hash).
  - `seed` bypasses recompute; empty seed is a no-op.
  - `tests/CMakeLists.txt` grants `test_content_hash` access to
    `src/` (internal headers), same pattern as `02_graph_smoke`.

**Alternatives considered.**
- **OpenSSL `EVP_DigestInit_ex` / libsodium `crypto_hash_sha256`**: more
  battle-tested than FFmpeg's AVSHA but adds a whole dependency to the
  link graph for ~40 LOC of wrapper. libavutil is already pulled for
  demux / mux / swscale; its sha256 is used in FFmpeg's own DASH and
  HLS code, so it's load-bearing enough. Rejected.
- **Hand-rolled SHA-256** (~120 lines of reference C): educational but
  zero test coverage for edge cases, and a determinism invariant around
  hashing is not a place to try cute. Rejected.
- **Computing hash at demux time** (inside `io::demux_kernel` as a side
  effect): would avoid the per-clip engine seed, but ties hash lifetime
  to the demuxer's open/close cycle and duplicates work across multiple
  clips referencing the same asset. The engine-level cache decouples
  lookup from demux. Rejected.
- **`me::Asset` as a first-class IR node** (id → Asset struct with uri /
  hash / colorSpace / metadata) instead of folding `content_hash` into
  `Clip`: the cleaner design for M2+ when multiple clips can reference
  the same asset. Deferred to a proper refactor cycle (PAIN_POINTS entry
  added); phase-1 only carries `Clip`-level data, so folding is OK for
  now — the hash ends up in the engine cache regardless.
- **C API `me_asset_hash(uri)`**: natural external surface for hosts
  that want to invalidate caches. Not required by the backlog bullet
  ("为后续 cache key 做准备" = foundation only), and adding a new
  exported symbol without a real consumer would premature-commit to a
  specific ABI shape. Defer. No new public symbol this cycle.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build build-rel
  -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON -DME_BUILD_TESTS=ON` both
  clean; `-Wall -Wextra -Wpedantic -Werror` green.
- `ctest --test-dir build` / `--test-dir build-rel` — 4/4 suites
  passing (test_content_hash new; existing test_status / test_engine /
  test_timeline_schema unchanged and still green).
- `01_passthrough` on the 2 s sample + `05_reencode` single-clip
  regression: both produce expected MP4 output (ffprobe 0 error).
- The loader now exercises `contentHash` parsing on any valid-looking
  JSON; existing JSONs that omit the field still load (field is
  optional). Non-conforming prefixes or non-hex bodies surface
  `ME_E_UNSUPPORTED` / `ME_E_PARSE` with last_error populated.

**License impact.** No new FetchContent / find_package. libavutil's
AVSHA is already a transitive dep of `FFMPEG::avutil` (LGPL, whitelist
confirmed in ARCHITECTURE.md).

**Registration.** Changes this cycle:
- TaskKindId / kernel registry — untouched.
- Resource factory — new `me::resource::AssetHashCache` instance owned
  by `me_engine` alongside `FramePool` / `CodecPool`. Construction
  order: after CodecPool, before Scheduler (destruction reverse).
- Orchestrator factory — untouched.
- Exported C API — no new symbols; `me_timeline_load_json` gains a
  passive side effect (seeds engine cache) with no observable change in
  return value / out-params.
- CMake / FetchContent — `src/CMakeLists.txt` adds two TUs
  (`resource/content_hash.cpp`, `resource/asset_hash_cache.cpp`);
  `tests/CMakeLists.txt` adds `test_content_hash` and grants it a
  `src/` include for internal headers.
- JSON schema — no field added; loader now *honors* the existing
  optional `assets[].contentHash`. `"sha256:..."` prefix enforced,
  other algorithms rejected with clear diagnostic.
- Timeline IR — `me::Clip` grows `std::string content_hash`.
