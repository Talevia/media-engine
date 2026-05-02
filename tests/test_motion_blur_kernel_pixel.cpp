/* test_motion_blur_kernel_pixel — M12 §157 (1/3). */
#include <doctest/doctest.h>

#include "compose/motion_blur_kernel.hpp"

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

TEST_CASE("apply_motion_blur_inplace: null buffer rejected") {
    me::MotionBlurEffectParams p;
    p.dx_px = 4; p.samples = 5;
    CHECK(me::compose::apply_motion_blur_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_motion_blur_inplace: bad samples rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::MotionBlurEffectParams p;
    p.dx_px = 4;
    p.samples = 0;
    CHECK(me::compose::apply_motion_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.samples = 65;
    CHECK(me::compose::apply_motion_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_motion_blur_inplace: samples=1 → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::MotionBlurEffectParams p;
    p.dx_px = 16; p.dy_px = 16; p.samples = 1;
    REQUIRE(me::compose::apply_motion_blur_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_motion_blur_inplace: zero direction → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::MotionBlurEffectParams p;
    p.dx_px = 0; p.dy_px = 0; p.samples = 9;
    REQUIRE(me::compose::apply_motion_blur_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_motion_blur_inplace: solid color preserved by any blur") {
    /* A uniformly colored image must remain unchanged after motion
     * blur — every tap reads the same value. */
    auto buf = make_solid(16, 16, 64, 73, 137, 211, 255);
    const auto snapshot = buf;
    me::MotionBlurEffectParams p;
    p.dx_px = 8; p.dy_px = 0; p.samples = 9;
    REQUIRE(me::compose::apply_motion_blur_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_motion_blur_inplace: sharp edge softens") {
    /* Build 16×1 row: left half black, right half white. After
     * horizontal motion blur with dx=8, samples=9, the edge column
     * should NOT be 0 or 255 anymore — must be in (0, 255). */
    std::vector<std::uint8_t> buf(16 * 4);
    for (int x = 0; x < 16; ++x) {
        const std::uint8_t v = (x < 8) ? 0 : 255;
        buf[x * 4 + 0] = v;
        buf[x * 4 + 1] = v;
        buf[x * 4 + 2] = v;
        buf[x * 4 + 3] = 255;
    }
    me::MotionBlurEffectParams p;
    p.dx_px = 8; p.dy_px = 0; p.samples = 9;
    REQUIRE(me::compose::apply_motion_blur_inplace(buf.data(), 16, 1, 64, p)
            == ME_OK);

    /* Pixels around the original edge must show intermediate values. */
    bool found_intermediate = false;
    for (int x = 4; x < 12; ++x) {
        const std::uint8_t r = buf[x * 4 + 0];
        if (r > 0 && r < 255) { found_intermediate = true; break; }
    }
    CHECK(found_intermediate);
}

TEST_CASE("apply_motion_blur_inplace: alpha averaged like RGB") {
    /* Half-row alpha=255, half alpha=0 → blurred middle should
     * have alpha somewhere in (0, 255). */
    std::vector<std::uint8_t> buf(16 * 4);
    for (int x = 0; x < 16; ++x) {
        buf[x * 4 + 0] = 100;
        buf[x * 4 + 1] = 100;
        buf[x * 4 + 2] = 100;
        buf[x * 4 + 3] = (x < 8) ? 0 : 255;
    }
    me::MotionBlurEffectParams p;
    p.dx_px = 8; p.dy_px = 0; p.samples = 9;
    REQUIRE(me::compose::apply_motion_blur_inplace(buf.data(), 16, 1, 64, p)
            == ME_OK);
    bool found_intermediate_alpha = false;
    for (int x = 4; x < 12; ++x) {
        const std::uint8_t a = buf[x * 4 + 3];
        if (a > 0 && a < 255) { found_intermediate_alpha = true; break; }
    }
    CHECK(found_intermediate_alpha);
}

TEST_CASE("apply_motion_blur_inplace: determinism") {
    auto a = make_solid(16, 16, 64, 200, 100, 50, 255);
    /* Add a step inside so blur isn't the no-op case. */
    for (int y = 0; y < 16; ++y) {
        for (int x = 8; x < 16; ++x) {
            a[y * 64 + x * 4 + 0] = 0;
        }
    }
    auto b = a;
    me::MotionBlurEffectParams p;
    p.dx_px = 6; p.dy_px = 2; p.samples = 7;
    REQUIRE(me::compose::apply_motion_blur_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_motion_blur_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(a == b);
}
