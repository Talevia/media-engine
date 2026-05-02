/*
 * test_chromatic_aberration_kernel_pixel — pixel regression
 * for the M12 §156 (3/5) chromatic-aberration kernel.
 */
#include <doctest/doctest.h>

#include "compose/chromatic_aberration_kernel.hpp"

#include <cstdint>
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

TEST_CASE("apply_chromatic_aberration_inplace: null buffer rejected") {
    me::ChromaticAberrationEffectParams p; p.red_dx = 2;
    CHECK(me::compose::apply_chromatic_aberration_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_chromatic_aberration_inplace: out-of-range shifts rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::ChromaticAberrationEffectParams p;
    p.red_dx = 33;
    CHECK(me::compose::apply_chromatic_aberration_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p = me::ChromaticAberrationEffectParams{};
    p.blue_dy = -33;
    CHECK(me::compose::apply_chromatic_aberration_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_chromatic_aberration_inplace: zero shifts → no-op") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::ChromaticAberrationEffectParams p;
    REQUIRE(me::compose::apply_chromatic_aberration_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_chromatic_aberration_inplace: red_dx=2 reads R from 2 columns to the right") {
    /* Build a 4x1 image where R increases left-to-right. */
    std::vector<std::uint8_t> buf(4 * 4);
    for (int x = 0; x < 4; ++x) {
        buf[x * 4 + 0] = static_cast<std::uint8_t>(50 * x);
        buf[x * 4 + 1] = 100;
        buf[x * 4 + 2] = 50;
        buf[x * 4 + 3] = 255;
    }
    me::ChromaticAberrationEffectParams p;
    p.red_dx = 2;
    REQUIRE(me::compose::apply_chromatic_aberration_inplace(buf.data(), 4, 1, 16, p)
            == ME_OK);

    /* Column 0: R reads from clamp(0+2, 4)=2 → 100 (50*2).
     * Column 1: R reads from clamp(1+2, 4)=3 → 150.
     * Column 2: R reads from clamp(2+2, 4)=3 (clamped) → 150.
     * Column 3: R reads from clamp(3+2, 4)=3 (clamped) → 150.
     * G/B/A unchanged. */
    CHECK(buf[0 * 4 + 0] == 100);
    CHECK(buf[1 * 4 + 0] == 150);
    CHECK(buf[2 * 4 + 0] == 150);
    CHECK(buf[3 * 4 + 0] == 150);
    /* G/B/A unchanged. */
    for (int x = 0; x < 4; ++x) {
        CHECK(buf[x * 4 + 1] == 100);
        CHECK(buf[x * 4 + 2] == 50);
        CHECK(buf[x * 4 + 3] == 255);
    }
}

TEST_CASE("apply_chromatic_aberration_inplace: blue_dy=1 reads B from row below") {
    /* 1x4 image where B increases top-to-bottom. */
    std::vector<std::uint8_t> buf(4 * 4);
    for (int y = 0; y < 4; ++y) {
        buf[y * 4 + 0] = 200;
        buf[y * 4 + 1] = 100;
        buf[y * 4 + 2] = static_cast<std::uint8_t>(50 * y);
        buf[y * 4 + 3] = 255;
    }
    me::ChromaticAberrationEffectParams p;
    p.blue_dy = 1;
    REQUIRE(me::compose::apply_chromatic_aberration_inplace(buf.data(), 1, 4, 4, p)
            == ME_OK);

    /* Row 0: B reads from clamp(0+1)=1 → 50.
     * Row 1: B reads from row 2 → 100.
     * Row 2: B reads from row 3 → 150.
     * Row 3: B reads from clamp(3+1, 4)=3 → 150 (clamped). */
    CHECK(buf[0 * 4 + 2] == 50);
    CHECK(buf[1 * 4 + 2] == 100);
    CHECK(buf[2 * 4 + 2] == 150);
    CHECK(buf[3 * 4 + 2] == 150);
}

TEST_CASE("apply_chromatic_aberration_inplace: solid color survives any shift") {
    auto buf = make_solid(16, 16, 64, 100, 200, 50, 255);
    const auto snapshot = buf;
    me::ChromaticAberrationEffectParams p;
    p.red_dx = 5; p.red_dy = -3;
    p.blue_dx = -7; p.blue_dy = 2;
    REQUIRE(me::compose::apply_chromatic_aberration_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}
