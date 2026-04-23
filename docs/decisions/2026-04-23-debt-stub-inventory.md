## 2026-04-23 — debt-stub-inventory (Milestone §M1 · Rubric §5.2)

**Context.** `scan-debt.sh` §R.5 signal #2 counts `return ME_E_UNSUPPORTED`
site-wide (currently 12). That number is noisy: it mixes real stubs
(`me_frame_*` returns nullptr, `me_cache_stats` returns zeros,
`Previewer::frame_at` is a pure M6 placeholder) with legitimate runtime
rejections (unsupported codec combo, missing HW encoder, spec validation
failures). iterate-gap needs a function-level view — "which API is still
a stub today, and what impl bullet does it map to" — to plan milestone
close-out. A blanket grep can't tell those apart.

**Decision.**
- Introduce an explicit `STUB:` comment convention. Every genuine stub
  carries one marker:

  ```
  /* STUB: <slug> — <short note> */
  ```

  Slug is kebab-case and SHOULD match a backlog `*-impl` bullet so
  cross-referencing is trivial (`grep STUB:.*frame-server-impl src/` ↔
  `grep frame-server-impl docs/BACKLOG.md`). The " — " separator is a
  convention, not parsed strictly; the tool treats everything after the
  slug as opaque note text.
- `tools/check_stubs.sh`: greps for `STUB:` markers under `src/`,
  parses path / line / slug / note via awk, emits a markdown table
  sorted by file+line so diffs between two runs isolate added/removed
  stubs. Always exits 0 — reporting tool, not a gate. Exit-gating
  would belong in CI (a separate downstream concern, not this skill's
  scope).
- Markers added this cycle (6 stubs total):
  - `src/api/cache.cpp` — 3 markers (`cache-stats-impl`,
    `cache-clear-impl`, `cache-invalidate-impl`).
  - `src/api/render.cpp` — 1 marker covering `me_frame_*` accessors
    (`frame-server-impl`).
  - `src/orchestrator/previewer.cpp` — 1 marker on `frame_at`
    (`frame-server-impl`, same slug — deduplication at the slug level
    is intentional: one milestone bullet, two call sites).
  - `src/orchestrator/thumbnailer.cpp` — 1 marker on the Timeline-path
    `png_at` (`composition-thumbnail-impl`, distinct from the already-
    implemented asset-level `me_thumbnail_png`).
- What's explicitly NOT marked: `return ME_E_UNSUPPORTED` paths inside
  implemented functions that reject bad inputs at runtime. Examples:
  `Exporter::export_to` rejecting unsupported codec combos,
  `reencode_mux` rejecting missing `h264_videotoolbox`. These are
  correct behavior, not missing impl.

**Alternatives considered.**
- **Blanket `grep return ME_E_UNSUPPORTED`** (what `scan-debt.sh` §2
  does): impossible to separate stubs from runtime rejects, and the
  count grew legitimately in this very cycle (reencode_pipeline.cpp
  added 3 valid rejection returns while probe-impl removed 8 real
  stubs — the raw count rose even though net stubs fell). Keeping
  scan-debt's raw count for the debt-regression curve but layering
  this marker-based view on top. Not rejected — complementary.
- **Use a distinct error code `ME_E_NOT_IMPLEMENTED`** (new enum
  value): type-system-level separation would let the compiler help.
  But it breaks the ABI-stable enum promise in CLAUDE.md invariant 10
  — adding a new enum value means existing `me_status_str` switches
  need updating across time, and enum numeric values must never
  collide. The added risk outweighs the gain; comment markers are
  opt-in, free, and zero ABI cost. Rejected.
- **YAML/JSON registry of stubs** (`tools/stubs.yaml`): single source
  of truth, but duplicated bookkeeping — the marker lives in code, the
  registry lives beside it, and drift between them is inevitable.
  "Markers are the registry" is simpler. Rejected.
- **Have the check_stubs.sh tool also cross-reference against
  BACKLOG.md bullets**: nice idea, but adds grammar fragility (what
  if the slug is mentioned in a decision or PAIN_POINTS rather than
  BACKLOG?). Defer — the eye-scan is fine at 6 stubs. If the count
  grows to 40+, revisit.
- **Gate CI on "stub count must not increase"**: strict but the raw
  count can legitimately rise (as this cycle shows) when new modules
  land with their initial placeholder. Better metric: "count of
  distinct slugs" — that only rises when a genuinely new gap opens.
  Defer gating until a CI harness exists to own this.

**Coverage.**
- `bash tools/check_stubs.sh` returns a clean 6-row table against the
  current tree:
  ```
  cache-stats-impl           src/api/cache.cpp:11
  cache-clear-impl           src/api/cache.cpp:19
  cache-invalidate-impl      src/api/cache.cpp:24
  frame-server-impl          src/api/render.cpp:89
  frame-server-impl          src/orchestrator/previewer.cpp:6
  composition-thumbnail-impl src/orchestrator/thumbnailer.cpp:7
  ```
  Output format is markdown; can paste directly into issues / commit
  bodies / decision docs.
- `cmake --build build` + `cmake --build build-rel
  -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON` both clean — annotations
  are pure comments; no code path changed.
- `ctest` Debug + Release: 5/5 suites pass unchanged.
- `01_passthrough` / `05_reencode` / `06_thumbnail` regressions: all
  still produce expected output (these examples don't hit the stubbed
  paths).

**License impact.** No new dependencies; shell tool uses system awk +
grep + date, all POSIX-standard.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory / orchestrator factory — untouched.
- Exported C API — no symbols added/removed; function bodies unchanged
  except for `STUB:` comment insertion.
- CMake / FetchContent — untouched.
- JSON schema — untouched.
- New tool: `tools/check_stubs.sh` (executable, shell script, POSIX awk).
- New convention: `/* STUB: <slug> — <note> */` comment format,
  documented in the tool's header.
