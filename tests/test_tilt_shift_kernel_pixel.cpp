/* test_tilt_shift_kernel_pixel — M12 §157 (3/3). */
#include <doctest/doctest.h>

#include "compose/tilt_shift_kernel.hpp"

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

TEST_CASE("apply_tilt_shift_inplace: null buffer rejected") {
    me::TiltShiftEffectParams p;
    p.max_blur_radius = 4;
    CHECK(me::compose::apply_tilt_shift_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tilt_shift_inplace: bad ranges rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::TiltShiftEffectParams p;
    p.max_blur_radius = 4;

    p.focal_y_min = 0.7f; p.focal_y_max = 0.5f;
    CHECK(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p.focal_y_min = 0.4f; p.focal_y_max = 0.6f;
    p.edge_softness = 0.0f;
    CHECK(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p.edge_softness = 1.5f;
    CHECK(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p.edge_softness = 0.2f;
    p.max_blur_radius = -1;
    CHECK(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p.max_blur_radius = 33;
    CHECK(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tilt_shift_inplace: max_blur_radius=0 → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::TiltShiftEffectParams p;
    p.max_blur_radius = 0;
    REQUIRE(me::compose::apply_tilt_shift_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_tilt_shift_inplace: solid color preserved") {
    auto buf = make_solid(16, 16, 64, 73, 137, 211, 255);
    const auto snapshot = buf;
    me::TiltShiftEffectParams p;
    p.focal_y_min = 0.4f; p.focal_y_max = 0.6f;
    p.edge_softness = 0.1f; p.max_blur_radius = 5;
    REQUIRE(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_tilt_shift_inplace: focal-band rows unchanged") {
    /* Build a noisy 16x16 image. After tilt-shift with focal band
     * [0.4, 0.6] (rows 6..9 in 16-row image), those rows should be
     * pixel-identical to input (r=0 there). */
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::size_t i = (y * 16 + x) * 4;
            buf[i + 0] = static_cast<std::uint8_t>((x * 13 + y * 7) & 0xFF);
            buf[i + 1] = static_cast<std::uint8_t>((x * 5) & 0xFF);
            buf[i + 2] = static_cast<std::uint8_t>((y * 11) & 0xFF);
            buf[i + 3] = 255;
        }
    }
    const auto snapshot = buf;
    me::TiltShiftEffectParams p;
    p.focal_y_min = 0.4f; p.focal_y_max = 0.6f;
    p.edge_softness = 0.1f; p.max_blur_radius = 5;
    REQUIRE(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    /* Row 6: y_norm = 6/15 = 0.4 → in band → r=0 → unchanged.
     * Row 9: y_norm = 9/15 = 0.6 → in band → r=0 → unchanged. */
    for (int y : {6, 7, 8, 9}) {
        for (int x = 0; x < 16; ++x) {
            const std::size_t i = (y * 16 + x) * 4;
            CHECK(buf[i + 0] == snapshot[i + 0]);
            CHECK(buf[i + 1] == snapshot[i + 1]);
            CHECK(buf[i + 2] == snapshot[i + 2]);
            CHECK(buf[i + 3] == snapshot[i + 3]);
        }
    }
}

TEST_CASE("apply_tilt_shift_inplace: rows outside band differ") {
    /* Same noisy image. Top row (y=0, y_norm=0) is fully outside
     * the band → should differ from input. */
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::size_t i = (y * 16 + x) * 4;
            buf[i + 0] = static_cast<std::uint8_t>((x * 13 + y * 7) & 0xFF);
            buf[i + 1] = static_cast<std::uint8_t>((x * 5) & 0xFF);
            buf[i + 2] = static_cast<std::uint8_t>((y * 11) & 0xFF);
            buf[i + 3] = 255;
        }
    }
    const auto snapshot = buf;
    me::TiltShiftEffectParams p;
    p.focal_y_min = 0.4f; p.focal_y_max = 0.6f;
    p.edge_softness = 0.1f; p.max_blur_radius = 5;
    REQUIRE(me::compose::apply_tilt_shift_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    bool top_row_differs = false;
    for (int x = 0; x < 16; ++x) {
        if (buf[x * 4 + 0] != snapshot[x * 4 + 0]) { top_row_differs = true; break; }
    }
    CHECK(top_row_differs);
}

TEST_CASE("apply_tilt_shift_inplace: determinism") {
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
    me::TiltShiftEffectParams p;
    p.focal_y_min = 0.3f; p.focal_y_max = 0.7f;
    p.edge_softness = 0.15f; p.max_blur_radius = 4;
    REQUIRE(me::compose::apply_tilt_shift_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_tilt_shift_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(a == b);
}
