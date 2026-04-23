## 2026-04-23 — determinism-regression-test (Milestone §M1 · Rubric §5.3)

**Context.** VISION §3.3 / §5.3 makes "same inputs → same bytes on the
software path" a non-negotiable invariant. Before this cycle nothing
actually enforced it. The passthrough-mux path is, in practice,
byte-deterministic today (ad-hoc check: two back-to-back `01_passthrough`
runs produce `cmp`-identical outputs — no wall-clock metadata leaked in),
but "trivially holds today" and "remains true under future changes" are
different claims. This cycle installs the tripwire.

**Decision.**
- New test suite `tests/test_determinism.cpp` (2 cases / 10 assertions):
  1. `passthrough is byte-deterministic across two independent renders` —
     creates two fresh engines (`me_engine_create` twice), each renders
     the same JSON timeline through the passthrough spec, slurps the two
     outputs, `CHECK(bytes1 == bytes2)`. Diverging bytes → `FAIL` with
     the first-differing offset for diagnosis.
  2. `passthrough determinism holds across engine restarts` — same shape
     but with ~100k malloc/free roundtrips between renders to advance the
     wall clock past any single-jiffy bucket. Catches regressions where
     someone adds wall-time-based metadata that happens to coincide when
     two renders fire within the same millisecond.
- Fixture generation via `find_program(FFMPEG_EXECUTABLE ffmpeg)` +
  `add_custom_command`:
  - Encodes 10 frames of `testsrc` at 320×240 @ 10 fps with `mpeg4` +
    `-q:v 5` → `build/tests/fixtures/determinism_input.mp4`. MPEG-4
    Part 2 picked because it's built into every LGPL-only libavcodec
    (no libx264 / VideoToolbox dependency), so CI machines with
    bare-LGPL ffmpeg still build the fixture.
  - Fixture is regenerated only if missing or older than
    `tests/CMakeLists.txt` — incremental builds don't pay the cost.
  - Path propagated to the test via `target_compile_definitions
    ME_TEST_FIXTURE_MP4="..."`. If ffmpeg isn't found at configure time,
    the define is absent and both test cases `MESSAGE`-skip with a
    visible note. Result: no hard dep, graceful degradation.
- Test body builds its own timeline JSON with `std::ostringstream` rather
  than shipping a fixture `.json` file — avoids an extra file to keep in
  sync with the test, but duplicates the "minimal valid timeline" shape
  that `test_timeline_schema.cpp` also hand-rolls. Captured in
  PAIN_POINTS as a consolidation candidate.
- No source changes to `passthrough_mux` — the invariant already holds
  (confirmed by the ad-hoc `cmp` check at cycle start, now codified).

**Alternatives considered.**
- **In-process libav fixture generator** (encode 10 MJPEG / MPEG-4 frames
  via `AVCodecContext` directly from C++): self-contained, no ffmpeg
  CLI dependency, ~60 lines of boilerplate. Rejected for this cycle
  because the existing `reencode_pipeline.cpp` already has ~600 lines
  of near-identical boilerplate — adding another copy right before the
  pending "AV_ RAII deleters dedup" refactor (already in PAIN_POINTS)
  would entrench the duplication. Once that refactor lands, a shared
  fixture helper is the natural follow-up — noted in PAIN_POINTS.
- **Compare a stable hash (sha256) of the two outputs** instead of raw
  `memcmp`: smaller failure message, no diff tooling. Rejected because
  a byte-offset diff is genuinely useful for triage (in practice you
  want to know "where did it diverge"), and the outputs are ~100 KB —
  `memcmp` cost is trivial.
- **Require `/tmp/input.mp4` to exist** (same fixture path the other
  examples already use): zero CMake plumbing, but couples the test to
  an ambient assumption about the dev machine. CI would have to pre-
  stage the file. The custom-command approach is more portable.
- **Freeze a golden fixture in git** (`tests/fixtures/determinism_input.mp4`
  as a committed binary blob): perfectly reproducible, but checks in a
  binary artifact and pins the fixture to whatever encoder version made
  it. The generate-on-build approach keeps the repo binary-free and
  auto-adapts to the host's encoder set.
- **Also gate `reencode_mux` under the same test**: h264_videotoolbox is
  a HW encoder — Apple doesn't guarantee bit-identical output across
  runs (documented in Apple's AVFoundation notes). VISION explicitly
  labels the re-encode path as "HW-accelerated, non-deterministic". So
  the determinism invariant is scoped to passthrough; re-encode has no
  expected-bytes to compare against. Rejected as out of scope.

**Coverage.**
- `cmake --build build -DME_BUILD_TESTS=ON` → fixture regenerated via
  system ffmpeg; `test_determinism` links and runs.
- `ctest --test-dir build` — 5/5 suites pass (new: test_determinism
  2 TC / 10 assertions). Debug + Release `-Werror` both clean; C++20
  `-Wall -Wextra -Wpedantic` all green.
- `01_passthrough` / `05_reencode` / `06_thumbnail` regressions
  unchanged; this cycle added tests only, no source-of-truth changes.
- Cross-check: ran `cmp` on two back-to-back `01_passthrough` outputs
  against the original sample — IDENTICAL (this was the check that
  motivated writing the test as a tripwire rather than a bug fix).

**License impact.** No new link-time dependency. The fixture uses
LGPL libavcodec's built-in `mpeg4` encoder; the system ffmpeg CLI is
a build-time-only tool, not part of the shipped binary.

**Registration.** Changes this cycle:
- `TaskKindId` / kernel registry — untouched.
- Resource factory / orchestrator factory — untouched.
- Exported C API — no new symbols.
- CMake: `find_program(FFMPEG_EXECUTABLE ffmpeg)` +
  `add_custom_command` producing `determinism_input.mp4`; new
  executable target `test_determinism`; new ctest case.
- JSON schema — untouched.
- New source: `tests/test_determinism.cpp`.
