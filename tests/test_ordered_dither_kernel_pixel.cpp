/* test_ordered_dither_kernel_pixel — M12 §156 (5/5). */
#include <doctest/doctest.h>

#include "compose/ordered_dither_kernel.hpp"

#include <cstdint>
#include <set>
#include <vector>

namespace {

std::vector<std::uint8_t> make_solid(int w, int h, std::size_t stride,
                                       std::uint8_t r, std::uint8_t g,
                                       std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> rgba(stride * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * stride +
                                   static_cast<std::size_t>(x) * 4;
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = a;
        }
    }
    return rgba;
}

}  // namespace

TEST_CASE("apply_ordered_dither_inplace: null buffer rejected") {
    me::OrderedDitherEffectParams p; p.matrix_size = 4; p.levels = 4;
    CHECK(me::compose::apply_ordered_dither_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_ordered_dither_inplace: bad matrix_size rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::OrderedDitherEffectParams p;
    p.levels = 4;
    p.matrix_size = 3;
    CHECK(me::compose::apply_ordered_dither_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.matrix_size = 16;
    CHECK(me::compose::apply_ordered_dither_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_ordered_dither_inplace: bad levels rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::OrderedDitherEffectParams p;
    p.matrix_size = 4;
    p.levels = 1;
    CHECK(me::compose::apply_ordered_dither_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.levels = 257;
    CHECK(me::compose::apply_ordered_dither_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_ordered_dither_inplace: levels=256 → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::OrderedDitherEffectParams p;  /* defaults: matrix_size=4, levels=256 */
    REQUIRE(me::compose::apply_ordered_dither_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_ordered_dither_inplace: solid mid-gray with levels=2 dithers") {
    /* On a 4x4 patch of mid-gray (128) with matrix_size=4 + levels=2,
     * the 4x4 Bayer matrix should produce a stipple of 0s and 255s
     * (not all-uniform like posterize). Verify both 0 and 255 appear. */
    auto buf = make_solid(4, 4, 16, 128, 128, 128, 255);
    me::OrderedDitherEffectParams p;
    p.matrix_size = 4;
    p.levels      = 2;
    REQUIRE(me::compose::apply_ordered_dither_inplace(buf.data(), 4, 4, 16, p)
            == ME_OK);

    std::set<std::uint8_t> r_values;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            r_values.insert(buf[y * 16 + x * 4 + 0]);
        }
    }
    /* Both buckets should be hit (binary output: 0 and 255). */
    CHECK(r_values.count(0)   == 1);
    CHECK(r_values.count(255) == 1);
    CHECK(r_values.size()     == 2);
}

TEST_CASE("apply_ordered_dither_inplace: alpha never modified") {
    auto buf = make_solid(8, 8, 32, 100, 100, 100, 77);
    me::OrderedDitherEffectParams p;
    p.matrix_size = 4;
    p.levels      = 4;
    REQUIRE(me::compose::apply_ordered_dither_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK(buf[(y * 32) + x * 4 + 3] == 77);
        }
    }
}

TEST_CASE("apply_ordered_dither_inplace: determinism") {
    auto a = make_solid(16, 16, 64, 200, 100, 50, 255);
    auto b = make_solid(16, 16, 64, 200, 100, 50, 255);
    me::OrderedDitherEffectParams p;
    p.matrix_size = 8;
    p.levels      = 6;
    REQUIRE(me::compose::apply_ordered_dither_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_ordered_dither_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(a == b);
}

TEST_CASE("apply_ordered_dither_inplace: 2x2 vs 8x8 patterns differ") {
    /* Same input + same levels but different matrix_size should yield
     * different outputs (different threshold structure). */
    auto a = make_solid(8, 8, 32, 100, 100, 100, 255);
    auto b = make_solid(8, 8, 32, 100, 100, 100, 255);
    me::OrderedDitherEffectParams p;
    p.levels = 4;
    p.matrix_size = 2;
    REQUIRE(me::compose::apply_ordered_dither_inplace(a.data(), 8, 8, 32, p)
            == ME_OK);
    p.matrix_size = 8;
    REQUIRE(me::compose::apply_ordered_dither_inplace(b.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(a != b);
}
