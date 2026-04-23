## 2026-04-23 — timeline-asset-map (Milestone §M1 · Rubric §5.1)

**Context.** `me::Timeline` held only `std::vector<Clip>`, and each
`Clip` carried a full `asset_uri` + `content_hash` pair inline. Two
practical consequences flagged by PAIN_POINTS 2026-04-23: the same
asset referenced by multiple clips duplicated both fields once per
clip; and callers that wanted asset-level metadata (URI, hash,
future colorSpace / container hints) had to loop through clips to
reach it, conflating per-clip and per-asset concerns. M2's
multi-track path + eventual OCIO integration both need asset-level
metadata to live on the Asset, not on each Clip that references it.

**Decision.**
- New `me::Asset { uri, content_hash }` struct in
  `src/timeline/timeline_impl.hpp`.
- `me::Timeline` grows `std::unordered_map<std::string, Asset>
  assets` keyed by the JSON asset id. Lookup-only (never iterated
  inside code that produces determinism-visible output), so unordered
  storage is safe per §3a.6. Explicit comment guards future readers.
- `me::Clip` drops `asset_uri` and `content_hash`, gains
  `asset_id` (single string). Loader guarantees non-empty after
  successful load: unknown asset_id returns `ME_E_PARSE`.
- `src/timeline/timeline_loader.cpp`: populates `tl.assets` directly
  (was a local `asset_map` that dropped in the stack frame); each
  clip record just carries `asset_id`. `contentHash` parsing and
  normalization unchanged.
- `src/api/timeline.cpp`: seeds `engine->asset_hashes` by iterating
  `tl.assets` (which contains each asset once, regardless of how
  many clips reference it) — cleaner than iterating clips + deduping
  and strictly more efficient for multi-clip same-asset timelines.
  Iteration order doesn't affect observable state because `seed()`
  is idempotent per URI.
- `src/orchestrator/exporter.cpp`: introduces a tiny
  `resolve_uri(tl, asset_id) → const std::string&` helper and uses
  `timeline.assets.at(asset_id).uri` to look up per-clip URIs for
  demux graph construction. Missing ids are a programmer error at
  this layer (loader rejected them) — `at()` throws
  `std::out_of_range` if ever violated, which the exporter's
  surrounding try/catch translates to `ME_E_INTERNAL`.
- `examples/03_timeline_segments/main.cpp`: internal segmentation
  smoke test that constructs `Clip` structs by hand — updated to
  use `.asset_id` instead of `.asset_uri`. No behavioral change.

**Alternatives considered.**
- **Keep the duplication; add a "dedup at seed time" pass in
  `me_timeline_load_json`**: would save the IR refactor but doesn't
  address the per-clip / per-asset concern conflation that OCIO and
  M2 multi-track both need fixed. Rejected — paying the refactor
  cost now, not later when more code depends on the shape.
- **`std::map` (ordered) instead of `std::unordered_map`**: ordered
  iteration is determinism-stable but the map is never iterated in
  determinism-critical code (seed is idempotent, lookup is by-key
  only). `unordered_map` is O(1) average, `map` is O(log N) — neither
  matters for <10 assets but the simpler semantic wins.
- **Inline `Asset` into `Clip` as `std::shared_ptr<Asset>`**:
  cleaner pointer semantics (no id-based lookup), but forces Clip
  copies to share asset state in ways that can bite when a Clip is
  modified in-place downstream. The id-map indirection matches what
  the JSON schema already looks like (assets[] + clips with
  assetId), so the IR now mirrors the schema's conceptual
  structure.
- **Keep Clip.asset_uri as a redundant denormalized field alongside
  asset_id**: denormalization often ends in the fields disagreeing
  after refactors. One source of truth.
- **Expose `me_timeline_asset_hash(tl, asset_id)` C API**: useful
  future addition but not required by this backlog item. No new
  symbol this cycle.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build
  build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON` — both clean.
- `ctest` Debug + Release: 6/6 suites pass. Critical suites that
  exercise the refactored IR:
  - `test_timeline_schema` — valid single-clip + multi-clip +
    negative paths still classify correctly; loader still rejects
    unknown assetId.
  - `test_content_hash` — seeded `contentHash` still populates the
    engine cache (now via `tl.assets` iteration).
  - `test_cache` — `me_cache_stats` reports the right
    entry_count / miss_count after timeline load.
  - `test_determinism` — byte-for-byte passthrough equality still
    holds (refactor is IR-level, not packet-level).
- `01_passthrough` — 2s MP4 unchanged.
- `01_passthrough` on multi-clip fixture — 4.02s output unchanged.
- `05_reencode` — h264+aac MP4 unchanged.
- `03_timeline_segments` — segmentation tests (single / abutting /
  gap / overlap / boundary-hash-determinism) all pass with the
  updated Clip builder.

**License impact.** No dependency changes. `<unordered_map>` was
already in the link graph.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory — untouched.
- Orchestrator factory — `Exporter::export_to` internals changed
  to look up URI via `tl.assets.at(asset_id)`; public signature
  stable.
- Exported C API — no new or removed symbols. JSON shape
  unchanged (asset-side had `id / uri / contentHash`; Clip-side
  had `assetId`); only the internal IR restructure.
- CMake — untouched.
- JSON schema — untouched.
- Internal IR: `me::Asset` new; `me::Timeline` gains `assets` map;
  `me::Clip` drops `asset_uri` + `content_hash`, gains `asset_id`.
