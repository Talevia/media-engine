# Testing conventions

How to write tests in this repo. Written after 12 doctest suites + 1 fixture generator shipped; codifies the patterns that worked so new suites don't re-invent them.

This is **how**. For *why* the project has tests at all, read `docs/VISION.md` §5.2 ("Engineering hygiene") and `docs/MILESTONES.md`'s M1 exit criterion on regression tripwires.

## Building + running

Tests are off by default. Turn them on at configure time:

```sh
cmake -B build -S . -DME_BUILD_TESTS=ON -DME_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Every test suite links `media_engine::media_engine` + `doctest::doctest` and is registered via `_test_suites` list in `tests/CMakeLists.txt`. One binary per suite — if one crashes, ctest keeps going on the others.

Run a single suite verbose:

```sh
build/tests/test_probe -s
```

`-s` (success) prints every assertion + `MESSAGE(...)` line; useful when hunting "did this case actually run or did it silently skip?".

## Framework: doctest (not gtest, not catch2)

Chosen because single header + faster compile than Catch2 + no separate runner binary. All the conventions below assume doctest's API:

- `TEST_CASE("human-readable title")` — the basic unit.
- `REQUIRE(expr)` — fatal; if false, rest of the case is skipped.
- `CHECK(expr)`  — non-fatal; failure recorded, case continues.
- `MESSAGE(x)`   — annotation, not an assertion.
- `FAIL("...")`  — unconditional failure with a message.

**Rule of thumb**: `REQUIRE` for "precondition that must hold or the rest of the test is meaningless" (e.g. engine creation, fixture path exists). `CHECK` for the actual claim you're testing. Favour many `CHECK`s over one compound `REQUIRE` — failed `CHECK`s all report; failed `REQUIRE` stops everything.

## Directory layout

```
tests/
  CMakeLists.txt                  — single source of the suite registry
  test_main.cpp                   — doctest entry, shared by every suite
  test_<area>.cpp                 — one suite per TU
  timeline_builder.hpp            — header-only fluent JSON helper (reused)
  fixtures/
    gen_fixture.cpp               — build-time deterministic MP4 generator
```

One `test_<area>.cpp` per functional area. Area can be "a C API group" (`test_probe`, `test_thumbnail`), "an internal subsystem" (`test_timeline_schema`, `test_timeline_segment`), "an invariant" (`test_determinism`, `test_asset_reuse`), or "a factory" (`test_color_pipeline`). Don't cram unrelated concerns into one suite — the ctest output should let "which functional area regressed?" be answerable at a glance.

## Three fixture patterns

### (a) `TimelineBuilder` — fluent JSON builder

For anything that exercises `me_timeline_load_json`. Lives in `tests/timeline_builder.hpp` (header-only; no dedicated TU). Default instance produces a minimal valid single-clip timeline; mutators add assets, clips, schema-version overrides, clip-level JSON fragments.

```cpp
namespace tb = me::tests::tb;

// Minimal valid
auto j = tb::minimal_video_clip().build();

// Negative case
auto bad = tb::minimal_video_clip().schema_version(2).build();

// Multi-clip, single-asset (for asset-reuse dedup)
auto multi = tb::TimelineBuilder()
    .add_asset(tb::AssetSpec{.id="a1", .uri="file:///x.mp4"})
    .add_clip(tb::ClipSpec{.clip_id="c1", .asset_id="a1"})
    .add_clip(tb::ClipSpec{
        .clip_id="c2", .asset_id="a1",
        .time_start_num=60, .time_dur_num=60, .source_dur_num=60})
    .build();
```

Override **all four** rational fields (`time_start_*`, `time_dur_*`, `source_dur_*`, `source_start_*` as needed) when you change any one — the loader enforces `timeRange.duration == sourceRange.duration`, and mismatched defaults hit that check first before whatever you actually wanted to test (learned this the hard way in `debt-test-multi-track-asset-reuse`).

Tests that need shapes the builder doesn't cover (multi-track, explicit unknown fields) embed raw JSON inline — `test_timeline_schema`'s multi-track case is the canonical example. Don't extend the builder for a one-off negative case; inline is cheaper than a wider builder API.

### (b) `ME_TEST_FIXTURE_MP4` — shared build-time MP4

`tests/fixtures/gen_fixture.cpp` builds a tiny deterministic MP4 (640×480 @ 25fps MPEG-4 Part 2, no audio, BITEXACT) via direct libavcodec calls; `tests/CMakeLists.txt` wires it as a `determinism_fixture` target. Every suite that wants a real decoder input adds:

```cmake
add_dependencies(test_foo determinism_fixture)
target_compile_definitions(test_foo PRIVATE
  ME_TEST_FIXTURE_MP4="${_fixture_mp4}")
```

And in the `.cpp`:

```cpp
#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

TEST_CASE("...") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }
    // ...
}
```

Shared fixture so regenerating doesn't cascade. If you need a fixture with different characteristics (audio track, tagged color metadata, rotated frames, longer duration), extend `gen_fixture.cpp` (add options or a new mode) rather than writing a one-off generator — keeps the single-source-of-truth property.

### (c) RAII handle guards

Every suite that touches C API handles uses local RAII structs to guarantee destruction even through `REQUIRE` fast-exits:

```cpp
struct EngineHandle { me_engine_t* p = nullptr; ~EngineHandle() { if (p) me_engine_destroy(p); } };
struct TimelineHandle { me_timeline_t* p = nullptr; ~TimelineHandle() { if (p) me_timeline_destroy(p); } };
struct InfoHandle { me_media_info_t* p = nullptr; ~InfoHandle() { if (p) me_media_info_destroy(p); } };
struct PngBuffer { uint8_t* data = nullptr; size_t size = 0; ~PngBuffer() { if (data) me_buffer_free(data); } };
```

Cheaper than `std::unique_ptr<T, void(*)(T*)>` and trivially readable. Don't reach for shared fixtures (`TEST_SUITE` scopes, singletons) — per-case construction is ~microseconds and removes the "did the prior case leave state behind?" question from debugging.

## Two skip patterns

**Skip when a fixture isn't available**: `MESSAGE("skipping: fixture not available"); return;` — used for `ME_TEST_FIXTURE_MP4`-dependent cases. CI hosts without ffmpeg-on-PATH (pre-`debt-fixture-gen-libav`) hit this; current fixture is always available so this branch is cold.

**Skip when a HW dependency is missing**: `test_determinism`'s h264/aac reencode case checks the render status and skips if `h264_videotoolbox` isn't available:

```cpp
const me_status_t s1 = render_with_spec(json, out1, "h264", "aac");
if (s1 == ME_E_UNSUPPORTED || s1 == ME_E_ENCODE) {
    MESSAGE("skipping reencode determinism test: h264_videotoolbox unavailable (status="
            << me_status_str(s1) << ")");
    return;
}
REQUIRE(s1 == ME_OK);
```

Linux CI hits this skip; mac dev-machine runs the case. Pattern: the first render call is the probe; skip **only** on `ME_E_UNSUPPORTED` / `ME_E_ENCODE`, treat anything else (including `ME_E_OK`) as "test can run"; REQUIRE the second render call's status strictly.

## Reaching into `src/`

Internal tests (`test_timeline_schema`, `test_timeline_segment`, `test_content_hash`, `test_color_pipeline`, `test_output_sink`) need internal headers. The per-suite CMake clause:

```cmake
target_include_directories(test_timeline_segment PRIVATE ${CMAKE_SOURCE_DIR}/src)
```

This is **per-suite**, not project-wide — keeps the public-header-only suites (`test_engine`, `test_probe`, `test_thumbnail`, `test_cache`, `test_asset_reuse`) honest about never touching `src/` types.

## Pinning error strings

For negative cases, assert the **status code** fully and the **error message** by substring:

```cpp
CHECK(load(eng, bad_json, &tl) == ME_E_UNSUPPORTED);
CHECK(std::string{me_engine_last_error(eng)}.find("exactly one track") != std::string::npos);
```

Substring match so future wording changes are deliberate — deleting/renaming a semantic token (e.g. dropping `"exactly one track"` entirely) makes the test fail; cosmetic reword of surrounding text doesn't. `test_timeline_schema` + `test_probe` + `test_thumbnail` all use this pattern.

## Byte-comparison assertions

`test_determinism` is the canonical example. Always slurp via a helper (`std::vector<unsigned char>`) and compare full content; on mismatch, scan for the first differing byte and `FAIL("outputs differ at byte offset " << i)`. Knowing where they diverge saves hours of bisecting.

Do **not** pixel-compare decoded frames — `sws_scale` filter defaults drift between FFmpeg minor versions. Assert structural invariants (PNG signature, IHDR dimensions, container tag, frame count, byte count) instead. `test_thumbnail` shows the PNG-header-parse pattern (8-byte signature + 16-byte IHDR chunk → no libpng needed).

## When to add a test

- A **decision-backed contract** lands (e.g. `asset-map` dedup, `determinism` across paths, API-extension null-safety). Add a test that would fail if the contract slips. `test_asset_reuse` + `test_determinism` + `test_probe` accessor null-safety cases are examples.
- A **factory / policy function** with no consumer yet (e.g. `me::color::make_pipeline()`). Tests force instantiation so header-only inline bodies get compile-checked under `-Werror`. `test_color_pipeline` is this pattern.
- An **IR-level function** (e.g. `timeline::segment()`) that's only covered via stdout examples. Port the example's scenarios to doctest. `test_timeline_segment` did this for `03_timeline_segments`.
- A **stub lifts to real impl** (`me_probe`, `me_thumbnail_png`). Pin the new happy-path + the negative paths it still maintains.

## When **not** to add a test

- **Private implementation detail** (e.g. "this lambda captures the right references"). Covered by integration tests via observable output; adding a test for internal shape blocks refactor.
- **The test would be a specification copy** (e.g. re-deriving `av_display_rotation_get`'s arithmetic instead of trusting FFmpeg). Ship an integration assertion against a known fixture instead.
- **Failure mode is already caught by ctest output** (e.g. "does the binary link?"). The build itself is the test.

## FAQ

**Why not a bigger framework (Catch2, gtest)?** doctest is single-header, compiles in <1s, and its macros are enough. We haven't hit a case where the framework is the bottleneck.

**Why one binary per suite?** A crash in `test_engine` shouldn't skip `test_probe`. ctest's output also becomes "which area failed?" at a glance.

**Why `ME_BUILD_TESTS=OFF` default?** The engine is a library; hosts building against it shouldn't pay for our test infra. CI / dev both flip it ON.

**Does every test need an architectural rationale in the commit body?** No — for pure coverage additions a one-line "filled an observability gap" in the commit body is enough. Commit bodies carry richer rationale (see `.claude/skills/iterate-gap/SKILL.md` §7) only when the change touches architecture / contracts / public API.
