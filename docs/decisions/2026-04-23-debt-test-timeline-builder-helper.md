## 2026-04-23 — debt-test-timeline-builder-helper (Milestone §M1-debt · Rubric §5.2)

**Context.** Four test suites (`test_timeline_schema.cpp`,
`test_determinism.cpp`, `test_cache.cpp`, `test_engine.cpp`) each
hand-rolled a "minimal valid timeline" JSON string in-situ, mutated via
`find + replace` for negative cases. PAIN_POINTS 2026-04-23 flagged this
twice: any schema rename (e.g. `frameRate → frame_rate`) would ripple
through N test files in lock-step, and mutation via substring-find is
brittle when another field happens to share a prefix.

**Decision.**
- New header-only `tests/timeline_builder.hpp` under `me::tests::tb`:
  - `AssetSpec { id, kind, uri, content_hash }` — POD struct with
    designated-initializer-friendly defaults.
  - `ClipSpec { clip_id, type, asset_id, time_* / source_* (num, den),
    extra }` — same. `extra` is a raw JSON-fragment injection point
    (must end with a comma) for negative cases that exercise
    rejection of disallowed fields like `effects` or `transform`
    without reshaping the builder API.
  - `TimelineBuilder` — fluent setters for schema/frame-rate/resolution/
    assets/clips, plus `with_clip_extra()` for single-clip templates.
    `build()` serializes via `std::ostringstream` with no JSON
    dependency (the engine's loader is what parses, so the builder
    just needs to produce syntactically valid strings).
  - `minimal_video_clip(uri)` — factory for the "one asset, one
    clip, defaults-everything-else" shape that most tests want.
- 4 test files migrated:
  - `test_timeline_schema.cpp` — 7 TCs: valid load, schema version
    rejection, malformed JSON, multi-clip contiguous, non-contiguous
    overlap, effects rejection, null-engine rejection, last_error
    population. Dropped the `kValidTimeline` const; each TC now
    constructs exactly the shape it needs. Negative mutation cases
    use builder setters (`schema_version(2)`, `with_clip_extra(...)`)
    instead of inline find/replace.
  - `test_determinism.cpp` — 2 TCs. Both use
    `TimelineBuilder().frame_rate(10,1).resolution(320,240).add_asset(...).add_clip(...).build()`
    parameterized with the test fixture path.
  - `test_cache.cpp` — `timeline_json(uri, hex_hash)` helper now
    delegates to the builder; 4 TCs unchanged.
  - `test_engine.cpp` — the "clear_error on success" TC's inline
    valid-timeline string replaced with
    `tb::minimal_video_clip("file:///tmp/x.mp4").build()`.
- `tests/CMakeLists.txt` — no changes needed. The builder is
  header-only and picked up automatically via the test's default
  include path (`tests/`).

**Alternatives considered.**
- **nlohmann::json-based builder** (tests already transitively link
  it via media_engine): cleaner semantics (object construction by key
  lookup) but drags a real JSON library into every test TU and adds
  compile time. The `ostringstream` string-concat approach has zero
  dependency cost and the output is still syntactically valid JSON
  (tests exercise rejection paths via intentional malformation, not
  accidental).
- **Template the builder on clip count** and force-correct time
  ranges: would save the user from remembering to bump
  `time_start_num` when adding a second clip. Rejected because the
  `test_timeline_schema.cpp` "non-contiguous clips" TC deliberately
  constructs an overlapping timeline to test rejection — forcing
  contiguity removes the ability to exercise that path.
- **Keep the builder as a static method `TimelineBuilder::minimal(...)`
  instead of a free function `minimal_video_clip()`**: both work;
  free function is one less `::` in call sites and matches how doctest
  users typically layer test helpers.
- **Make the extra-fragment field structured** (e.g. a
  `std::vector<std::pair<std::string, std::string>>` for custom
  fields): over-engineered for today's 1 use site (effects rejection
  test). Raw string injection is honest about its purpose.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` + `cmake --build
  build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON
  -DME_BUILD_TESTS=ON` — both clean.
- `ctest` Debug + Release: 6/6 suites pass. Relevant counts after
  migration:
  - `test_timeline_schema`: 7 TCs (unchanged count, 1 file's worth
    of inline JSON gone).
  - `test_determinism`: 2 TCs.
  - `test_cache`: 5 TCs.
  - `test_engine`: 6 TCs.
- `01_passthrough` / `05_reencode` / `06_thumbnail` — untouched by
  this cycle (test-only refactor).

**License impact.** No dependencies added or removed. `std::ostringstream`
and `std::vector` are in every TU already via test_main.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory / orchestrator factory — untouched.
- Exported C API — untouched.
- CMake — untouched (header-only helper).
- JSON schema — untouched.
- New test-internal header: `tests/timeline_builder.hpp`.
