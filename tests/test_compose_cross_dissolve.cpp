/*
 * test_compose_cross_dissolve — pixel-level math tripwire for
 * me::compose::cross_dissolve.
 *
 * Pins the linear-lerp contract across the transition curve:
 *   t=0.0 → dst == from (exact copy)
 *   t=1.0 → dst == to   (exact copy)
 *   t=0.5 → dst is the midpoint (within 1 LSB rounding tolerance)
 *   arbitrary t produces predictable values across all 4 RGBA channels.
 *
 * Determinism: same inputs + t produce bit-identical output (no FMA /
 * SIMD drift guard).
 */
#include <doctest/doctest.h>

#include "compose/cross_dissolve.hpp"

#include <array>
#include <cstdint>
#include <vector>

using me::compose::cross_dissolve;

namespace {

/* 1×1 RGBA helper. */
std::array<uint8_t, 4> px(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return {r, g, b, a};
}

void dissolve_1x1(std::array<uint8_t, 4>&       dst,
                  const std::array<uint8_t, 4>& from,
                  const std::array<uint8_t, 4>& to,
                  float                         t) {
    cross_dissolve(dst.data(), from.data(), to.data(),
                   /*width=*/1, /*height=*/1, /*stride_bytes=*/4, t);
}

}  // namespace

TEST_CASE("cross_dissolve t=0 copies from into dst") {
    auto from = px(10, 20, 30, 255);
    auto to   = px(200, 100, 50, 128);
    auto dst  = px(0, 0, 0, 0);
    dissolve_1x1(dst, from, to, 0.0f);
    CHECK(dst[0] == 10);
    CHECK(dst[1] == 20);
    CHECK(dst[2] == 30);
    CHECK(dst[3] == 255);
}

TEST_CASE("cross_dissolve t=1 copies to into dst") {
    auto from = px(10, 20, 30, 255);
    auto to   = px(200, 100, 50, 128);
    auto dst  = px(0, 0, 0, 0);
    dissolve_1x1(dst, from, to, 1.0f);
    CHECK(dst[0] == 200);
    CHECK(dst[1] == 100);
    CHECK(dst[2] == 50);
    CHECK(dst[3] == 128);
}

TEST_CASE("cross_dissolve t=0.5 produces midpoint (within rounding tolerance)") {
    auto from = px(0,   0,   0,   0);
    auto to   = px(255, 255, 255, 255);
    auto dst  = px(42, 42, 42, 42);   /* pre-populated junk to detect overwrite */
    dissolve_1x1(dst, from, to, 0.5f);
    /* 0 + 255 * 0.5 = 127.5 → 128 (round-half-away-from-zero via lroundf). */
    CHECK(dst[0] == 128);
    CHECK(dst[1] == 128);
    CHECK(dst[2] == 128);
    CHECK(dst[3] == 128);
}

TEST_CASE("cross_dissolve t=0.25 skews toward from") {
    auto from = px(100, 100, 100, 255);
    auto to   = px(200, 200, 200, 255);
    auto dst  = px(0, 0, 0, 0);
    dissolve_1x1(dst, from, to, 0.25f);
    /* 100 * 0.75 + 200 * 0.25 = 75 + 50 = 125. */
    CHECK(dst[0] == 125);
    CHECK(dst[1] == 125);
    CHECK(dst[2] == 125);
    CHECK(dst[3] == 255);
}

TEST_CASE("cross_dissolve t=0.75 skews toward to") {
    auto from = px(100, 100, 100, 255);
    auto to   = px(200, 200, 200, 255);
    auto dst  = px(0, 0, 0, 0);
    dissolve_1x1(dst, from, to, 0.75f);
    /* 100 * 0.25 + 200 * 0.75 = 25 + 150 = 175. */
    CHECK(dst[0] == 175);
    CHECK(dst[1] == 175);
    CHECK(dst[2] == 175);
    CHECK(dst[3] == 255);
}

TEST_CASE("cross_dissolve out-of-range t clamps to [0,1]") {
    auto from = px(50, 50, 50, 50);
    auto to   = px(150, 150, 150, 150);

    auto dst_neg = px(0, 0, 0, 0);
    dissolve_1x1(dst_neg, from, to, -0.5f);   /* clamp to 0 → dst = from */
    CHECK(dst_neg[0] == 50);

    auto dst_over = px(0, 0, 0, 0);
    dissolve_1x1(dst_over, from, to, 1.5f);   /* clamp to 1 → dst = to */
    CHECK(dst_over[0] == 150);
}

TEST_CASE("cross_dissolve operates across all 4 channels independently") {
    auto from = px(10, 100, 200, 50);
    auto to   = px(200, 100, 10, 250);   /* note: green stays 100 → dst green should stay 100 */
    auto dst  = px(0, 0, 0, 0);
    dissolve_1x1(dst, from, to, 0.5f);
    /* R: 10 + 190*0.5 = 105; G: unchanged 100; B: 200 - 190*0.5 = 105;
     * A: 50 + 200*0.5 = 150. lroundf may round 105.0 → 105, 100.0 → 100. */
    CHECK(dst[0] == 105);
    CHECK(dst[1] == 100);
    CHECK(dst[2] == 105);
    CHECK(dst[3] == 150);
}

TEST_CASE("cross_dissolve determinism — same inputs + t produce byte-identical dst") {
    std::vector<uint8_t> from(64), to(64);
    for (size_t i = 0; i < 64; ++i) { from[i] = static_cast<uint8_t>(i * 3 + 1); to[i] = static_cast<uint8_t>(i * 7 + 2); }

    std::vector<uint8_t> dst1(64, 0), dst2(64, 0);
    cross_dissolve(dst1.data(), from.data(), to.data(), /*w=*/4, /*h=*/4, /*stride=*/16, 0.37f);
    cross_dissolve(dst2.data(), from.data(), to.data(), /*w=*/4, /*h=*/4, /*stride=*/16, 0.37f);
    CHECK(dst1 == dst2);
}

TEST_CASE("cross_dissolve 0-size buffer is a no-op") {
    uint8_t nothing = 0;
    cross_dissolve(&nothing, &nothing, &nothing, /*w=*/0, /*h=*/100, /*stride=*/0, 0.5f);
    cross_dissolve(&nothing, &nothing, &nothing, /*w=*/100, /*h=*/0, /*stride=*/400, 0.5f);
    CHECK(nothing == 0);
}
