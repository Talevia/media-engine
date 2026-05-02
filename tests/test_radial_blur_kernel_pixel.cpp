/* test_radial_blur_kernel_pixel — M12 §157 (2/3). */
#include <doctest/doctest.h>

#include "compose/radial_blur_kernel.hpp"

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

TEST_CASE("apply_radial_blur_inplace: null buffer rejected") {
    me::RadialBlurEffectParams p;
    p.intensity = 0.1f; p.samples = 5;
    CHECK(me::compose::apply_radial_blur_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_radial_blur_inplace: bad samples rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::RadialBlurEffectParams p;
    p.intensity = 0.1f;
    p.samples = 0;
    CHECK(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.samples = 65;
    CHECK(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_radial_blur_inplace: bad ranges rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::RadialBlurEffectParams p;
    p.samples = 5;
    p.intensity = -0.01f;
    CHECK(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.intensity = 1.5f;
    CHECK(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.intensity = 0.1f;
    p.center_x = -0.5f;
    CHECK(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.center_x = 0.5f;
    p.center_y = 1.5f;
    CHECK(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_radial_blur_inplace: samples=1 → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::RadialBlurEffectParams p;
    p.intensity = 0.5f; p.samples = 1;
    REQUIRE(me::compose::apply_radial_blur_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_radial_blur_inplace: intensity=0 → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::RadialBlurEffectParams p;
    p.intensity = 0.0f; p.samples = 9;
    REQUIRE(me::compose::apply_radial_blur_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_radial_blur_inplace: solid color preserved") {
    /* Uniform input → uniform output regardless of intensity. */
    auto buf = make_solid(16, 16, 64, 73, 137, 211, 255);
    const auto snapshot = buf;
    me::RadialBlurEffectParams p;
    p.intensity = 0.3f; p.samples = 11;
    REQUIRE(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_radial_blur_inplace: center pixel unchanged") {
    /* At the radial center, the pixel-to-center vector is zero, so
     * every tap reads the center pixel itself → output = input there. */
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    /* Random-ish pattern so a center mismatch would be detectable. */
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::size_t i = (y * 16 + x) * 4;
            buf[i + 0] = static_cast<std::uint8_t>((x * 7 + y * 3) & 0xFF);
            buf[i + 1] = static_cast<std::uint8_t>((x * 11) & 0xFF);
            buf[i + 2] = static_cast<std::uint8_t>((y * 13) & 0xFF);
            buf[i + 3] = 255;
        }
    }
    /* Center at (8, 8) — 0.5 * (16-1) = 7.5 → round to 8. */
    const std::uint8_t center_r = buf[(8 * 16 + 8) * 4 + 0];
    const std::uint8_t center_g = buf[(8 * 16 + 8) * 4 + 1];
    const std::uint8_t center_b = buf[(8 * 16 + 8) * 4 + 2];

    me::RadialBlurEffectParams p;
    p.center_x = 0.5f; p.center_y = 0.5f;
    p.intensity = 0.5f; p.samples = 9;
    REQUIRE(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);

    CHECK(buf[(8 * 16 + 8) * 4 + 0] == center_r);
    CHECK(buf[(8 * 16 + 8) * 4 + 1] == center_g);
    CHECK(buf[(8 * 16 + 8) * 4 + 2] == center_b);
}

TEST_CASE("apply_radial_blur_inplace: alpha averaged like RGB") {
    /* Half image alpha=255, half alpha=0 → blurred middle should
     * have intermediate alpha. */
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::size_t i = (y * 16 + x) * 4;
            buf[i + 0] = 100;
            buf[i + 1] = 100;
            buf[i + 2] = 100;
            buf[i + 3] = (x < 8) ? 0 : 255;
        }
    }
    me::RadialBlurEffectParams p;
    p.center_x = 0.5f; p.center_y = 0.5f;
    p.intensity = 0.4f; p.samples = 11;
    REQUIRE(me::compose::apply_radial_blur_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    bool found_intermediate = false;
    for (int y = 6; y < 10; ++y) {
        for (int x = 6; x < 10; ++x) {
            const std::uint8_t a = buf[(y * 16 + x) * 4 + 3];
            if (a > 0 && a < 255) { found_intermediate = true; break; }
        }
    }
    CHECK(found_intermediate);
}

TEST_CASE("apply_radial_blur_inplace: determinism") {
    std::vector<std::uint8_t> a(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::size_t i = (y * 16 + x) * 4;
            a[i + 0] = static_cast<std::uint8_t>((x * 17 + y * 5) & 0xFF);
            a[i + 1] = 100;
            a[i + 2] = static_cast<std::uint8_t>((y * 23) & 0xFF);
            a[i + 3] = 255;
        }
    }
    auto b = a;
    me::RadialBlurEffectParams p;
    p.center_x = 0.4f; p.center_y = 0.6f;
    p.intensity = 0.2f; p.samples = 7;
    REQUIRE(me::compose::apply_radial_blur_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_radial_blur_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(a == b);
}
