/*
 * test_tone_curve_kernel_pixel — pixel regression for the M12
 * §155 (1/4) tone-curve kernel
 * (`me::compose::apply_tone_curve_inplace`).
 *
 * Coverage:
 *   - Argument-shape rejects (null buffer, bad dims, undersized
 *     stride, 1-point curves).
 *   - Empty-curve no-op (every channel empty → input bytes
 *     unchanged).
 *   - Identity curve (2 points: (0,0) and (255,255)) → input
 *     bytes unchanged on the affected channel.
 *   - Linear-mid curve ((0,0), (255,128)) → output = round(in / 2).
 *   - Inverse curve ((0,255), (255,0)) → output = 255 - in.
 *   - Per-channel independence: only the R curve is set; G/B
 *     pass through unchanged.
 *   - Alpha is never modified.
 */
#include <doctest/doctest.h>

#include "compose/tone_curve_kernel.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace {

me::ToneCurveEffectParams identity_params() {
    me::ToneCurveEffectParams p;
    p.r = {{0, 0}, {255, 255}};
    p.g = {{0, 0}, {255, 255}};
    p.b = {{0, 0}, {255, 255}};
    return p;
}

std::vector<std::uint8_t> make_gradient(int w, int h, std::size_t stride) {
    std::vector<std::uint8_t> rgba(stride * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * stride +
                                   static_cast<std::size_t>(x) * 4;
            rgba[i + 0] = static_cast<std::uint8_t>(x);  /* R = column */
            rgba[i + 1] = static_cast<std::uint8_t>(y);  /* G = row */
            rgba[i + 2] = 128;                            /* B = mid */
            rgba[i + 3] = 200;                            /* A sentinel */
        }
    }
    return rgba;
}

}  // namespace

TEST_CASE("apply_tone_curve_inplace: null buffer rejected") {
    std::string err;
    me::ToneCurveEffectParams p = identity_params();
    CHECK(me::compose::apply_tone_curve_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tone_curve_inplace: non-positive dimensions rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::ToneCurveEffectParams p = identity_params();
    CHECK(me::compose::apply_tone_curve_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_tone_curve_inplace(buf.data(), 16, 0, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tone_curve_inplace: undersized stride rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::ToneCurveEffectParams p = identity_params();
    CHECK(me::compose::apply_tone_curve_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tone_curve_inplace: 1-point curve rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::ToneCurveEffectParams p;
    p.r = {{128, 128}};  /* single point — malformed */
    CHECK(me::compose::apply_tone_curve_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tone_curve_inplace: all-empty curves → no-op") {
    auto buf      = make_gradient(16, 16, 64);
    const auto snapshot = buf;
    me::ToneCurveEffectParams p;  /* all curves empty */
    REQUIRE(me::compose::apply_tone_curve_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_tone_curve_inplace: identity curve preserves bytes") {
    auto buf      = make_gradient(16, 16, 64);
    const auto snapshot = buf;
    me::ToneCurveEffectParams p = identity_params();
    REQUIRE(me::compose::apply_tone_curve_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_tone_curve_inplace: linear-mid curve halves R/G/B; alpha unchanged") {
    auto buf = make_gradient(16, 16, 64);
    me::ToneCurveEffectParams p;
    /* (0, 0) → (255, 128): output = round(in * 128 / 255). The
     * round-half-up integer divide produces 128 = (255*128 + 127) / 255 = 128
     * for input 255, and 64 = (128*128 + 127) / 255 = 64 for input 128. */
    p.r = {{0, 0}, {255, 128}};
    p.g = {{0, 0}, {255, 128}};
    p.b = {{0, 0}, {255, 128}};
    REQUIRE(me::compose::apply_tone_curve_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);

    /* Pixel (0, 0): input (R=0, G=0, B=128, A=200) → R/G stay 0,
     * B halves to ~64 (within rounding), A unchanged. */
    auto rgba_at = [&](int x, int y) {
        const std::size_t i = static_cast<std::size_t>(y) * 64 +
                               static_cast<std::size_t>(x) * 4;
        return std::array<std::uint8_t, 4>{
            buf[i + 0], buf[i + 1], buf[i + 2], buf[i + 3]
        };
    };
    auto p00 = rgba_at(0, 0);
    CHECK(p00[0] == 0);
    CHECK(p00[1] == 0);
    /* B = 128 → ~64. Allow ±1 for rounding. */
    CHECK(p00[2] >= 63);
    CHECK(p00[2] <= 65);
    CHECK(p00[3] == 200);  /* alpha untouched */

    /* Pixel (15, 15): input R=15 → ~7-8, G=15 → ~7-8, B=128 → ~64. */
    auto p15 = rgba_at(15, 15);
    CHECK(p15[0] >= 7);
    CHECK(p15[0] <= 8);
    CHECK(p15[3] == 200);
}

TEST_CASE("apply_tone_curve_inplace: inverse curve flips R only; G/B/A unchanged") {
    auto buf      = make_gradient(16, 16, 64);
    me::ToneCurveEffectParams p;
    /* R: (0, 255) → (255, 0). Output = 255 - in. */
    p.r = {{0, 255}, {255, 0}};
    /* G/B left empty → unchanged. */
    REQUIRE(me::compose::apply_tone_curve_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);

    /* Pixel (5, 7): original (R=5, G=7, B=128, A=200) →
     * R = 255 - 5 = 250, G/B/A unchanged. */
    const std::size_t i = 7 * 64 + 5 * 4;
    CHECK(buf[i + 0] == 250);
    CHECK(buf[i + 1] == 7);
    CHECK(buf[i + 2] == 128);
    CHECK(buf[i + 3] == 200);
}

TEST_CASE("apply_tone_curve_inplace: piecewise-linear 3-point curve") {
    /* R curve: (0,0) → (128,32) → (255,255).
     * For input 128, output should be 32. For input 64
     * (mid of first segment), output ~16. For input 192
     * (mid of second segment, slope 223/127 ≈ 1.756), output
     * ≈ 32 + 64 * 223 / 127 ≈ 144. */
    me::ToneCurveEffectParams p;
    p.r = {{0, 0}, {128, 32}, {255, 255}};

    std::vector<std::uint8_t> buf(4 * 4);  /* 1 px @ 4 bytes, 4 rows of test inputs */
    /* Pack 4 test pixels at known input R values. */
    const int test_inputs[4] = {0, 64, 128, 192};
    for (int i = 0; i < 4; ++i) {
        buf[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(test_inputs[i]);
        buf[static_cast<std::size_t>(i) * 4 + 1] = 0;
        buf[static_cast<std::size_t>(i) * 4 + 2] = 0;
        buf[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    REQUIRE(me::compose::apply_tone_curve_inplace(buf.data(), 4, 1, 16, p)
            == ME_OK);

    /* Input 0 → 0. */
    CHECK(buf[0] == 0);
    /* Input 128 → 32 (exact knot). */
    CHECK(buf[8] == 32);
    /* Input 64 → ~16 (half of first segment). Allow ±1. */
    CHECK(buf[4] >= 15);
    CHECK(buf[4] <= 17);
    /* Input 192 → ~144 (half of second segment from 128/32 to 255/255). */
    CHECK(buf[12] >= 142);
    CHECK(buf[12] <= 146);
}
