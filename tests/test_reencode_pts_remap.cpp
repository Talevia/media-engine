/*
 * test_reencode_pts_remap — unit tests for
 * `me::orchestrator::detail::remap_source_pts_to_output`, the
 * helper at the heart of the vfr-av-sync fix.
 *
 * Pure math on int64 + AVRational; no libav context / codecs /
 * streams. Tests cover:
 *   - First frame anchored to segment_base (output 0 when base=0).
 *   - CFR source preserves even spacing through identity tb.
 *   - VFR source preserves irregular spacing through rescale.
 *   - Multi-segment: segment N starts after segment N-1 ends.
 *   - Time-base rescale is correct when src_tb != out_tb.
 */
#include <doctest/doctest.h>

#include "orchestrator/reencode_video.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

TEST_CASE("remap_source_pts_to_output: first frame of first segment is base") {
    /* src_pts = first_src_pts → src_offset = 0 → output = base. */
    const int64_t out = me::orchestrator::detail::remap_source_pts_to_output(
        /*src_pts=*/100, /*first_src_pts=*/100,
        /*src_tb=*/AVRational{1, 30},
        /*out_tb=*/AVRational{1, 30},
        /*segment_base=*/0);
    CHECK(out == 0);
}

TEST_CASE("remap_source_pts_to_output: CFR source preserves spacing") {
    /* Simulate 30fps CFR input at tb={1,30}: frames at 0, 1, 2, 3.
     * out_tb=={1,30}: identity rescale. Expected output: 0,1,2,3. */
    const AVRational tb{1, 30};
    const int64_t base = 0;
    const int64_t first = 0;
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(0, first, tb, tb, base) == 0);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(1, first, tb, tb, base) == 1);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(2, first, tb, tb, base) == 2);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(3, first, tb, tb, base) == 3);
}

TEST_CASE("remap_source_pts_to_output: VFR input preserves irregular intervals") {
    /* VFR-ish: frames at source pts 0, 1, 3, 4, 7 (irregular gaps).
     * Identity tb. Output should be 0, 1, 3, 4, 7 — intervals
     * preserved, not flattened to CFR. */
    const AVRational tb{1, 30};
    const int64_t first = 0;
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(0, first, tb, tb, 0) == 0);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(1, first, tb, tb, 0) == 1);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(3, first, tb, tb, 0) == 3);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(4, first, tb, tb, 0) == 4);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(7, first, tb, tb, 0) == 7);
}

TEST_CASE("remap_source_pts_to_output: first_src_pts anchors to 0") {
    /* Source starts at pts=1000 (e.g., seek landed mid-stream).
     * first_src_pts=1000 → output starts at 0 with correct
     * relative offsets. */
    const AVRational tb{1, 30};
    const int64_t first = 1000;
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(1000, first, tb, tb, 0) == 0);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(1001, first, tb, tb, 0) == 1);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(1005, first, tb, tb, 0) == 5);
}

TEST_CASE("remap_source_pts_to_output: segment_base adds cumulative prior duration") {
    /* Segment 2 starts after segment 1's 60 ticks in out_tb.
     * First frame of segment 2 (relative) → output = 60. */
    const AVRational tb{1, 30};
    const int64_t seg_base = 60;  /* prior segment ran 0..60 */
    const int64_t first = 500;    /* segment 2 source started at pts 500 */
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(500, first, tb, tb, seg_base) == 60);
    CHECK(me::orchestrator::detail::remap_source_pts_to_output(502, first, tb, tb, seg_base) == 62);
}

TEST_CASE("remap_source_pts_to_output: time-base rescale when tbs differ") {
    /* Source tb = {1, 30000}, output tb = {1, 30000/1001} would be
     * a canonical VFR source → 29.97fps CFR output case. Simpler:
     * src tb = {1, 90000} (MP4 typical), out tb = {1, 30}. Frame
     * every 3003 source ticks = 1/29.97s = 1 output tick when
     * rescaled. Actually for rescale: 3003 * 30 / 90000 = 1.001;
     * int div = 1. */
    const AVRational src_tb{1, 90000};
    const AVRational out_tb{1, 30};
    const int64_t first = 0;
    /* Source pts 3003 → output 3003 * 30 / 90000 = 1 (rounded). */
    const int64_t r = me::orchestrator::detail::remap_source_pts_to_output(
        3003, first, src_tb, out_tb, 0);
    CHECK(r == 1);
    /* Source pts 30030 → output = 30030 * 30 / 90000 = 10. */
    const int64_t r2 = me::orchestrator::detail::remap_source_pts_to_output(
        30030, first, src_tb, out_tb, 0);
    CHECK(r2 == 10);
}

TEST_CASE("remap_source_pts_to_output: source stays monotonic when input does") {
    /* Paranoid: input PTS monotonically increasing → output must
     * also be monotonically increasing. Critical invariant for
     * decoder ordering downstream. */
    const AVRational tb{1, 30};
    const int64_t first = 0;
    int64_t last = -1;
    for (int64_t i = 0; i < 100; ++i) {
        /* Irregular but monotonic source pattern: i + floor(i/3). */
        const int64_t src = i + i / 3;
        const int64_t out = me::orchestrator::detail::remap_source_pts_to_output(
            src, first, tb, tb, 0);
        CHECK(out > last);
        last = out;
    }
}
