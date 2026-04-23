/*
 * test_timeline_segment — doctest coverage for me::timeline::segment().
 *
 * `timeline::segment()` is the IR-to-orchestrator boundary: every
 * orchestrator (Previewer / Exporter / CompositionThumbnailer) walks
 * the segment list to decide which per-segment Graph to compile. The
 * `03_timeline_segments` example covers the same scenarios with its own
 * hand-rolled CHECK macro + stdout output, but there was no CI tripwire
 * until this suite — a refactor of the event-time dedup or the "clip
 * active in segment" predicate would slide through without noise.
 *
 * The cases below mirror 03_timeline_segments' scenarios (empty, single
 * clip, two abutting clips, gap-then-clip, boundary_hash determinism)
 * ported to doctest assertions. We deliberately do NOT duplicate the
 * overlapping-clip case yet — the loader forbids overlaps so exercising
 * that path from a test today would require hand-building a Timeline
 * with invalid IR, which is out of scope for this coverage cycle.
 */
#include <doctest/doctest.h>

#include "timeline/segmentation.hpp"
#include "timeline/timeline_impl.hpp"

#include <utility>
#include <vector>

using me::timeline::ClipRef;
using me::timeline::Segment;
using me::timeline::segment;

namespace {

bool rat_eq(me_rational_t a, me_rational_t b) {
    return a.num * b.den == b.num * a.den;
}

me::Timeline make_tl(std::vector<me::Clip> clips, me_rational_t duration) {
    me::Timeline tl;
    tl.frame_rate = {30, 1};
    tl.width      = 1920;
    tl.height     = 1080;
    tl.duration   = duration;
    tl.clips      = std::move(clips);
    return tl;
}

me::Clip mk_clip(me_rational_t start, me_rational_t dur, const char* asset_id = "x") {
    me::Clip c;
    c.asset_id      = asset_id;
    c.time_start    = start;
    c.time_duration = dur;
    return c;
}

}  // namespace

TEST_CASE("segment() on empty timeline returns zero segments") {
    /* Loader rejects empty timelines at the public API, but the IR-level
     * function is exposed to orchestrator fallbacks that might legitimately
     * hold a zero-duration Timeline (e.g. a caller inspecting an error
     * snapshot). Contract: zero segments, not a crash. */
    CHECK(segment(make_tl({}, {0, 1})).empty());
}

TEST_CASE("segment() on a single clip covering the full duration yields one segment") {
    auto tl = make_tl(
        {mk_clip({0, 1}, {2, 1})},
        {2, 1});

    const auto segs = segment(tl);
    REQUIRE(segs.size() == 1);
    CHECK(rat_eq(segs[0].start, {0, 1}));
    CHECK(rat_eq(segs[0].end,   {2, 1}));
    REQUIRE(segs[0].active_clips.size() == 1);
    CHECK(segs[0].active_clips[0].idx == 0);
}

TEST_CASE("segment() on two abutting clips yields two segments sharing only the boundary") {
    auto tl = make_tl(
        {mk_clip({0, 1}, {1, 1}, "a"),
         mk_clip({1, 1}, {1, 1}, "b")},
        {2, 1});

    const auto segs = segment(tl);
    REQUIRE(segs.size() == 2);

    CHECK(rat_eq(segs[0].start, {0, 1}));
    CHECK(rat_eq(segs[0].end,   {1, 1}));
    REQUIRE(segs[0].active_clips.size() == 1);
    CHECK(segs[0].active_clips[0].idx == 0);

    CHECK(rat_eq(segs[1].start, {1, 1}));
    CHECK(rat_eq(segs[1].end,   {2, 1}));
    REQUIRE(segs[1].active_clips.size() == 1);
    CHECK(segs[1].active_clips[0].idx == 1);

    /* Different active clip sets → different boundary hashes. */
    CHECK(segs[0].boundary_hash != segs[1].boundary_hash);
}

TEST_CASE("segment() emits explicit empty segments for gaps around clips") {
    /* Timeline duration = 3s; clip is active only on [1, 2). Expect
     * three segments: [0, 1) empty, [1, 2) clip, [2, 3) empty. Gaps
     * inside a single-track timeline don't land via the loader (it
     * requires contiguous clips), but the segment() algorithm is IR-
     * level and M2 compose will introduce genuine gaps between clips on
     * the same track. Pinning the gap shape now keeps the algorithm
     * honest. */
    auto tl = make_tl(
        {mk_clip({1, 1}, {1, 1}, "mid")},
        {3, 1});

    const auto segs = segment(tl);
    REQUIRE(segs.size() == 3);

    CHECK(rat_eq(segs[0].start, {0, 1}));
    CHECK(rat_eq(segs[0].end,   {1, 1}));
    CHECK(segs[0].active_clips.empty());

    CHECK(rat_eq(segs[1].start, {1, 1}));
    CHECK(rat_eq(segs[1].end,   {2, 1}));
    REQUIRE(segs[1].active_clips.size() == 1);
    CHECK(segs[1].active_clips[0].idx == 0);

    CHECK(rat_eq(segs[2].start, {2, 1}));
    CHECK(rat_eq(segs[2].end,   {3, 1}));
    CHECK(segs[2].active_clips.empty());

    /* Both gap segments share the same (empty) active set → same hash.
     * This is the cache-key correctness guarantee: disjoint segments
     * with identical active sets reuse one compiled Graph. */
    CHECK(segs[0].boundary_hash == segs[2].boundary_hash);
    CHECK(segs[0].boundary_hash != segs[1].boundary_hash);
}

TEST_CASE("segment() boundary_hash is stable across Timeline instances with same active set") {
    /* Determinism tripwire: same Clip layout → same boundary hashes,
     * regardless of which Timeline object the clips live in.
     * `segment()` should be a pure function of the Timeline shape. */
    auto tl1 = make_tl(
        {mk_clip({0, 1}, {1, 1}, "a")},
        {1, 1});
    auto tl2 = make_tl(
        {mk_clip({0, 1}, {1, 1}, "a")},
        {1, 1});

    const auto segs1 = segment(tl1);
    const auto segs2 = segment(tl2);
    REQUIRE(segs1.size() == 1);
    REQUIRE(segs2.size() == 1);
    CHECK(segs1[0].boundary_hash == segs2[0].boundary_hash);
}
