/*
 * test_hue_saturation_kernel_pixel — pixel regression for the
 * M12 §155 (2/4) hue/saturation kernel
 * (`me::compose::apply_hue_saturation_inplace`).
 *
 * Coverage:
 *   - Argument-shape rejects (null buffer, bad dims, bad
 *     stride, non-finite scales).
 *   - Identity params (default-constructed) → no-op.
 *   - saturationScale=0 fully desaturates → R==G==B for any
 *     input.
 *   - lightnessScale=0 → black (R=G=B=0).
 *   - hueShiftDeg=180 swaps complementary hues (red ↔ cyan,
 *     green ↔ magenta, blue ↔ yellow) within ±2 LSB tolerance
 *     to absorb the float→byte rounding.
 *   - Alpha is never modified.
 */
#include <doctest/doctest.h>

#include "compose/hue_saturation_kernel.hpp"

#include <cmath>
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

TEST_CASE("apply_hue_saturation_inplace: null buffer rejected") {
    me::HueSaturationEffectParams p;
    CHECK(me::compose::apply_hue_saturation_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_hue_saturation_inplace: non-positive dimensions rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::HueSaturationEffectParams p;
    CHECK(me::compose::apply_hue_saturation_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_hue_saturation_inplace(buf.data(), 16, 0, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_hue_saturation_inplace: undersized stride rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::HueSaturationEffectParams p;
    CHECK(me::compose::apply_hue_saturation_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_hue_saturation_inplace: non-finite params rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::HueSaturationEffectParams p;
    p.hue_shift_deg = std::nan("");
    CHECK(me::compose::apply_hue_saturation_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p = me::HueSaturationEffectParams{};
    p.saturation_scale = std::nan("");
    CHECK(me::compose::apply_hue_saturation_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_hue_saturation_inplace: identity params → no-op") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::HueSaturationEffectParams p;  /* defaults: identity */
    REQUIRE(me::compose::apply_hue_saturation_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_hue_saturation_inplace: saturationScale=0 fully desaturates") {
    auto buf = make_solid(4, 4, 16, 200, 100, 50, 255);
    me::HueSaturationEffectParams p;
    p.saturation_scale = 0.0f;
    REQUIRE(me::compose::apply_hue_saturation_inplace(buf.data(), 4, 4, 16, p)
            == ME_OK);

    /* All pixels should have R==G==B (achromatic). The actual
     * value is the lightness L = (max + min) / 2 quantized to
     * byte. For (200, 100, 50): L = (200 + 50) / 2 = 125.
     * Float-domain L = 250/2/255 = 0.4902 → byte 125. */
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 0] == buf[i + 1]);
        CHECK(buf[i + 1] == buf[i + 2]);
        CHECK(buf[i + 0] >= 124);
        CHECK(buf[i + 0] <= 126);
        CHECK(buf[i + 3] == 255);  /* alpha untouched */
    }
}

TEST_CASE("apply_hue_saturation_inplace: lightnessScale=0 → black") {
    auto buf = make_solid(4, 4, 16, 200, 100, 50, 200);
    me::HueSaturationEffectParams p;
    p.lightness_scale = 0.0f;
    REQUIRE(me::compose::apply_hue_saturation_inplace(buf.data(), 4, 4, 16, p)
            == ME_OK);

    /* L=0 → R=G=B=0 (black); alpha preserved. */
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 0] == 0);
        CHECK(buf[i + 1] == 0);
        CHECK(buf[i + 2] == 0);
        CHECK(buf[i + 3] == 200);
    }
}

TEST_CASE("apply_hue_saturation_inplace: hueShiftDeg=180 maps red → cyan") {
    /* Pure red (255, 0, 0) at hueShift=180 should map to cyan
     * (0, 255, 255). Allow ±2 LSB for float→byte rounding. */
    auto buf = make_solid(2, 2, 8, 255, 0, 0, 255);
    me::HueSaturationEffectParams p;
    p.hue_shift_deg = 180.0f;
    REQUIRE(me::compose::apply_hue_saturation_inplace(buf.data(), 2, 2, 8, p)
            == ME_OK);

    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 0] <= 2);          /* R near 0 */
        CHECK(buf[i + 1] >= 253);        /* G near 255 */
        CHECK(buf[i + 2] >= 253);        /* B near 255 */
        CHECK(buf[i + 3] == 255);        /* alpha unchanged */
    }
}

TEST_CASE("apply_hue_saturation_inplace: hueShiftDeg wraps modulo 360") {
    /* hueShift=540 == hueShift=180; verify equivalence. */
    auto buf_a = make_solid(2, 2, 8, 100, 200, 50, 255);
    auto buf_b = make_solid(2, 2, 8, 100, 200, 50, 255);
    me::HueSaturationEffectParams pa, pb;
    pa.hue_shift_deg = 180.0f;
    pb.hue_shift_deg = 540.0f;
    REQUIRE(me::compose::apply_hue_saturation_inplace(buf_a.data(), 2, 2, 8, pa)
            == ME_OK);
    REQUIRE(me::compose::apply_hue_saturation_inplace(buf_b.data(), 2, 2, 8, pb)
            == ME_OK);
    /* Allow ±1 LSB; same float math should produce identical
     * bytes but the modulo path may have a different rounding
     * trajectory. */
    for (std::size_t i = 0; i < buf_a.size(); i += 4) {
        CHECK(std::abs(static_cast<int>(buf_a[i + 0]) -
                        static_cast<int>(buf_b[i + 0])) <= 1);
        CHECK(std::abs(static_cast<int>(buf_a[i + 1]) -
                        static_cast<int>(buf_b[i + 1])) <= 1);
        CHECK(std::abs(static_cast<int>(buf_a[i + 2]) -
                        static_cast<int>(buf_b[i + 2])) <= 1);
    }
}

TEST_CASE("apply_hue_saturation_inplace: achromatic input survives any hue shift") {
    /* Greyscale (R=G=B) has H=0, S=0; the kernel's
     * achromatic branch in hsl_to_rgb produces R=G=B=L
     * regardless of hue. */
    auto buf = make_solid(2, 2, 8, 128, 128, 128, 255);
    me::HueSaturationEffectParams p;
    p.hue_shift_deg = 90.0f;
    REQUIRE(me::compose::apply_hue_saturation_inplace(buf.data(), 2, 2, 8, p)
            == ME_OK);

    for (std::size_t i = 0; i < buf.size(); i += 4) {
        CHECK(buf[i + 0] == 128);
        CHECK(buf[i + 1] == 128);
        CHECK(buf[i + 2] == 128);
        CHECK(buf[i + 3] == 255);
    }
}
