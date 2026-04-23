## 2026-04-23 — cache-stats-impl (Milestone §M1 · Rubric §5.3)

**Context.** Last M1 exit criterion: `me_cache_stats` must return real
hit/miss/entry_count values, "配合至少一层 asset 级缓存". Prior
implementation returned a zeroed struct with a `STUB:` marker
(`cache-stats-impl`), and `me_cache_clear` / `me_cache_invalidate_asset`
were no-ops. AssetHashCache (shipped 2026-04-23) already tracks entries
by URI, so the data source exists; counters and entrypoints just
needed wiring.

**Decision.**
- **AssetHashCache counters**: add `hits_` / `misses_` atomics,
  incremented only in `get_or_compute` (seed bypasses them — seeding is
  "trusted cache warmup", not a cache access). New accessors
  `hit_count()`, `miss_count()`. Counters are monotonic across the
  engine lifetime — `clear()` drops entries but not counters (see
  "Alternatives" below).
- **AssetHashCache operations**:
  - `clear()` — drops every entry; `size()` → 0.
  - `invalidate_by_hash(hex)` — iterates the map, erases entries whose
    stored value matches `hex` (case-insensitive against the
    lowercase-normalized stored values); returns count removed. O(N)
    but N is the asset count (small).
- **FramePool**: add `reset_counters()` (clears bytes_used +
  acquisitions atomics). The pool doesn't actually hold onto allocated
  buffers — `acquire` hands out a fresh `FrameHandle` every call, so
  "clear" is a counter reset, not a memory drop. Called by
  `me_cache_clear`.
- **`src/api/cache.cpp` rewrite** (replaces the three `STUB:` returns):
  - `me_cache_stats`: pulls `memory_bytes_used` /
    `memory_bytes_limit` from `FramePool::stats()`; `entry_count` /
    `hit_count` / `miss_count` from `AssetHashCache`. Disk stats
    remain zero + "-1 unlimited" until M6 disk cache lands.
  - `me_cache_clear`: calls `FramePool::reset_counters()` +
    `AssetHashCache::clear()`. Returns `ME_OK`.
  - `me_cache_invalidate_asset`: accepts both `"sha256:<hex>"` and
    bare `<hex>` — strips the schema prefix, hands the rest to
    `AssetHashCache::invalidate_by_hash`.
- **Null arg handling**: `me_cache_stats(NULL, ...)` /
  `me_cache_stats(eng, NULL)` → `ME_E_INVALID_ARG`. The previous stub
  accepted `engine=NULL` (it wrote zeros and returned OK); the new
  impl is stricter — NULL engine is now rejected, since the stats
  literally can't be read without one. ABI-compatible change (a
  callback site that was passing NULL to the old stub would have seen
  `{0,...,-1,...}`; now sees `ME_E_INVALID_ARG`). No known callers pass
  NULL so this is a net tightening.
- **Stub markers removed**: `cache-stats-impl`, `cache-clear-impl`,
  `cache-invalidate-impl` slugs no longer appear under `bash
  tools/check_stubs.sh`. Stub count drops from 6 → 3 (the remaining 3
  are all M6 frame-server-related).

**Tests.** New `tests/test_cache.cpp` (5 cases / 18 assertions):
1. Fresh engine → stats all zero.
2. NULL args → `ME_E_INVALID_ARG`.
3. `contentHash`-seeded timeline → `entry_count == 1`, `miss_count ==
   0` (seed bypasses compute).
4. `me_cache_clear` drops entries back to zero.
5. `me_cache_invalidate_asset` with `sha256:<hex>` form removes only
   the matching entry; bare hex form also accepted; non-matching hash
   leaves entries intact.

**Alternatives considered.**
- **Reset counters on `me_cache_clear`**: surface-plausible but makes
  the counters useless for long-running hosts that periodically clear
  to reclaim memory — they'd lose all history. Counters intentionally
  survive clears; hosts that want a clean slate can restart the
  engine. Rejected (counters are monotonic).
- **Compute hit/miss via `peek()` vs `get_or_compute()` callsite
  distinction**: the current callsites in the engine (loader seed +
  future cache lookup) are few and well-defined. Counting only in
  `get_or_compute` is the right grain — `peek` is a silent diagnostic.
  Kept.
- **Report `me_cache_stats.disk_bytes_limit = 0` when no disk cache
  configured**: clearer to callers, but the header documents `-1` as
  "unlimited". Returning `-1` uniformly preserves the "no limit
  observed" semantic. Kept `-1`.
- **Implement FramePool LRU + per-handle reclaim**: needed for a real
  frame cache, not an M1 exit criterion. Deferred to M6
  `frame-server-impl` via the backlog. `FramePool::acquire` still
  allocates fresh; bytes_used tracks cumulative allocation.
- **Walk orchestrator state in `invalidate_asset` to drop in-flight
  render job caches**: correct per API.md's "transitively reference"
  language, but today there are no such intermediate caches (no frame
  cache, no effect-output cache). Adding a walker now would be dead
  code. Revisit with M6 frame cache. Current impl handles asset-hash
  layer only; noted in the function comment.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build build-rel
  -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON -DME_BUILD_TESTS=ON` —
  both clean.
- `ctest` Debug + Release: 6/6 suites pass (new test_cache: 5 TC /
  18 assertions).
- `bash tools/check_stubs.sh` — count dropped 6 → 3; all three remaining
  stubs carry M6 slugs.
- `01_passthrough` regression — produces valid MP4 as before.
- `me_cache_stats` smoke: `me_cache_stats(eng, &s)` on a post-load
  timeline returns `entry_count == 1`, `miss_count == 0` (seeded),
  `hit_count == 0`.

**License impact.** No new dependencies. Counters use
`std::atomic<int64_t>` — `<atomic>` was already in the frame pool
header.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory — `AssetHashCache` grows counters + `clear` +
  `invalidate_by_hash`; `FramePool` grows `reset_counters`.
  Construction order in `me_engine_create` unchanged.
- Orchestrator — untouched.
- Exported C API — no new or removed symbols. Behavior change:
  `me_cache_stats(NULL, ...)` now returns `ME_E_INVALID_ARG` instead
  of `ME_OK + zeros`; `me_cache_clear` now clears cache state
  (previously no-op); `me_cache_invalidate_asset` now removes
  matching AssetHashCache entries (previously no-op).
- CMake — `tests/CMakeLists.txt` gains `test_cache` in the suite list.
- JSON schema — untouched.
- `STUB:` markers removed from `src/api/cache.cpp` for all three
  entries (cache-stats-impl, cache-clear-impl,
  cache-invalidate-impl).
