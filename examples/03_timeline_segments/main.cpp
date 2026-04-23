/*
 * 03_timeline_segments — validates timeline::segment() on hand-built Timelines.
 *
 * Uses INTERNAL headers; like 02_graph_smoke, this is an engine-internal
 * validation tool, not a public API demo. Public loaders currently only
 * accept single-clip timelines — this test goes directly against the IR.
 *
 * Scenarios:
 *   1. Empty timeline → 0 segments
 *   2. Single clip [0, 2s] with duration 2s → 1 segment [0, 2)
 *   3. Two abutting clips [0, 1), [1, 2) → 2 segments
 *   4. Gap [0, 1) + clip [1, 2) with duration 3 → 3 segments: gap, clip, tail-gap
 *   5. Overlapping clips [0, 2), [1, 3) → 3 segments: A only, A+B overlap, B only
 *   6. Boundary_hash determinism: same active set → same hash, regardless of
 *      segment time range
 */
#include "timeline/segmentation.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstdio>
#include <cstdlib>

using namespace me;
using namespace me::timeline;

namespace {

bool rat_eq(me_rational_t a, me_rational_t b) {
    return a.num * b.den == b.num * a.den;
}

Timeline make_tl(std::vector<Clip> clips, me_rational_t duration) {
    Timeline tl;
    tl.frame_rate = {30, 1};
    tl.width = 1920; tl.height = 1080;
    tl.duration = duration;
    tl.clips = std::move(clips);
    return tl;
}

Clip mk_clip(me_rational_t start, me_rational_t dur, const char* asset_id = "x") {
    return Clip{.asset_id = asset_id, .time_start = start, .time_duration = dur};
}

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "  FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } } while (0)

int test_empty() {
    auto segs = segment(make_tl({}, {0, 1}));
    CHECK(segs.empty());
    std::fprintf(stderr, "  test_empty: OK (0 segments)\n");
    return 0;
}

int test_single_clip() {
    auto tl = make_tl({ mk_clip({0,1}, {2,1}) }, {2,1});
    auto segs = segment(tl);
    CHECK(segs.size() == 1);
    CHECK(rat_eq(segs[0].start, {0,1}));
    CHECK(rat_eq(segs[0].end,   {2,1}));
    CHECK(segs[0].active_clips.size() == 1);
    CHECK(segs[0].active_clips[0].idx == 0);
    std::fprintf(stderr, "  test_single_clip: OK\n");
    return 0;
}

int test_two_abutting() {
    auto tl = make_tl({
        mk_clip({0,1}, {1,1}),
        mk_clip({1,1}, {1,1}),
    }, {2,1});
    auto segs = segment(tl);
    CHECK(segs.size() == 2);
    CHECK(segs[0].active_clips.size() == 1 && segs[0].active_clips[0].idx == 0);
    CHECK(segs[1].active_clips.size() == 1 && segs[1].active_clips[0].idx == 1);
    std::fprintf(stderr, "  test_two_abutting: OK\n");
    return 0;
}

int test_gap_plus_clip() {
    /* clip at [1,2), timeline duration 3: expect [0,1) gap, [1,2) clip, [2,3) gap */
    auto tl = make_tl({ mk_clip({1,1}, {1,1}) }, {3,1});
    auto segs = segment(tl);
    CHECK(segs.size() == 3);
    CHECK(segs[0].active_clips.empty());
    CHECK(segs[1].active_clips.size() == 1 && segs[1].active_clips[0].idx == 0);
    CHECK(segs[2].active_clips.empty());
    std::fprintf(stderr, "  test_gap_plus_clip: OK (%zu segments: gap/clip/gap)\n", segs.size());
    return 0;
}

int test_overlap() {
    /* A: [0,2), B: [1,3) → expect [0,1) A, [1,2) A+B, [2,3) B */
    auto tl = make_tl({
        mk_clip({0,1}, {2,1}, "A"),
        mk_clip({1,1}, {2,1}, "B"),
    }, {3,1});
    auto segs = segment(tl);
    CHECK(segs.size() == 3);
    CHECK(segs[0].active_clips.size() == 1 && segs[0].active_clips[0].idx == 0);
    CHECK(segs[1].active_clips.size() == 2);
    CHECK(segs[2].active_clips.size() == 1 && segs[2].active_clips[0].idx == 1);
    std::fprintf(stderr, "  test_overlap: OK (A / A+B / B)\n");
    return 0;
}

int test_boundary_hash_determinism() {
    /* Two non-adjacent segments with the same active set (same clip idx) must
     * have the same boundary_hash so their Graph can be cached once and
     * reused across multiple time ranges. */
    auto tl = make_tl({
        mk_clip({0,1}, {1,1}),   /* clip 0 active [0,1) */
        mk_clip({2,1}, {1,1}),   /* clip 1 active [2,3) */
    }, {4,1});  /* gap [1,2) and [3,4) both have empty active set */
    auto segs = segment(tl);

    /* Expect: [0,1) clip0, [1,2) empty, [2,3) clip1, [3,4) empty. */
    CHECK(segs.size() == 4);
    /* [1,2) and [3,4) have same active set (empty) → same hash */
    CHECK(segs[1].boundary_hash == segs[3].boundary_hash);
    /* [0,1) and [2,3) have different active sets → different hashes */
    CHECK(segs[0].boundary_hash != segs[2].boundary_hash);
    /* [0,1) and [1,2) have different active sets → different hashes */
    CHECK(segs[0].boundary_hash != segs[1].boundary_hash);
    std::fprintf(stderr, "  test_boundary_hash_determinism: OK\n");
    return 0;
}

}  // namespace

int main() {
    std::fprintf(stderr, "03_timeline_segments — timeline-segmentation validation\n");
    int rc = 0;
    rc |= test_empty();
    rc |= test_single_clip();
    rc |= test_two_abutting();
    rc |= test_gap_plus_clip();
    rc |= test_overlap();
    rc |= test_boundary_hash_determinism();
    std::fprintf(stderr, rc == 0 ? "ALL PASS\n" : "FAIL\n");
    return rc;
}
