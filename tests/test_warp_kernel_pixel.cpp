/* test_warp_kernel_pixel — M12 §158 (1/2). */
#include <doctest/doctest.h>

#include "compose/warp_kernel.hpp"

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

TEST_CASE("apply_warp_inplace: null buffer rejected") {
    me::WarpEffectParams p;
    p.control_points.push_back({0.5f, 0.5f, 0.6f, 0.6f});
    CHECK(me::compose::apply_warp_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_warp_inplace: too many control points rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::WarpEffectParams p;
    for (int i = 0; i < 33; ++i) {
        p.control_points.push_back({0.5f, 0.5f, 0.5f, 0.5f});
    }
    CHECK(me::compose::apply_warp_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_warp_inplace: out-of-range coord rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::WarpEffectParams p;
    p.control_points.push_back({1.5f, 0.5f, 0.5f, 0.5f});
    CHECK(me::compose::apply_warp_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p.control_points.clear();
    p.control_points.push_back({0.5f, -0.1f, 0.5f, 0.5f});
    CHECK(me::compose::apply_warp_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_warp_inplace: empty control_points → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::WarpEffectParams p;  /* no control points */
    REQUIRE(me::compose::apply_warp_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_warp_inplace: src == dst control points → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::WarpEffectParams p;
    p.control_points.push_back({0.25f, 0.25f, 0.25f, 0.25f});
    p.control_points.push_back({0.75f, 0.75f, 0.75f, 0.75f});
    REQUIRE(me::compose::apply_warp_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_warp_inplace: solid color preserved") {
    auto buf = make_solid(16, 16, 64, 73, 137, 211, 255);
    const auto snapshot = buf;
    me::WarpEffectParams p;
    p.control_points.push_back({0.3f, 0.3f, 0.5f, 0.5f});
    p.control_points.push_back({0.7f, 0.7f, 0.5f, 0.5f});
    REQUIRE(me::compose::apply_warp_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_warp_inplace: non-identity warp changes pixels") {
    /* Build a 16x16 image with a horizontal step (left half black,
     * right half white). Add one control point that shifts (0.5,0.5)
     * → (0.4, 0.5) (pulls input from x=0.5 to appear at x=0.4 in
     * output). The image should change. */
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::uint8_t v = (x < 8) ? 0 : 255;
            const std::size_t i = (y * 16 + x) * 4;
            buf[i + 0] = v;
            buf[i + 1] = v;
            buf[i + 2] = v;
            buf[i + 3] = 255;
        }
    }
    const auto snapshot = buf;
    me::WarpEffectParams p;
    p.control_points.push_back({0.7f, 0.5f, 0.3f, 0.5f});
    REQUIRE(me::compose::apply_warp_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf != snapshot);
}

TEST_CASE("apply_warp_inplace: control-point pinning") {
    /* Build a noisy image. With one control point (src=0.25,0.25)
     * → (0.5, 0.5), the output pixel near (0.5, 0.5) should equal
     * the input pixel near (0.25, 0.25). With 16x16 and rounded
     * pixel coords: dst=(7.5, 7.5) → output pixel at (7,7) or (8,8)
     * is close to input pixel at (3.75, 3.75) i.e. (4, 4). */
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const std::size_t i = (y * 16 + x) * 4;
            buf[i + 0] = static_cast<std::uint8_t>((x * 16 + y) & 0xFF);
            buf[i + 1] = 100;
            buf[i + 2] = static_cast<std::uint8_t>((y * 16 + x) & 0xFF);
            buf[i + 3] = 255;
        }
    }
    /* Sample at (4, 4) of input snapshot. */
    const std::uint8_t expected_r = buf[(4 * 16 + 4) * 4 + 0];

    me::WarpEffectParams p;
    /* Only one control point — IDW with a single point gives a
     * uniform displacement everywhere. dst=(0.5,0.5), src=(0.25,0.25)
     * → constant displacement (-0.25*15, -0.25*15) = (-3.75, -3.75)
     * applied to every output pixel. So output pixel (8, 8) reads
     * input at (8-3.75, 8-3.75) = (4.25, 4.25). */
    p.control_points.push_back({0.25f, 0.25f, 0.5f, 0.5f});
    REQUIRE(me::compose::apply_warp_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);

    /* Check that output at (8, 8) is close to input at (4.25, 4.25) =
     * bilinear of (4,4)/(5,4)/(4,5)/(5,5). Should be near
     * expected_r within ±32 (bilinear blend tolerance). */
    const std::uint8_t got_r = buf[(8 * 16 + 8) * 4 + 0];
    CHECK(std::abs(static_cast<int>(got_r) - static_cast<int>(expected_r)) <= 32);
}

TEST_CASE("apply_warp_inplace: determinism") {
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
    me::WarpEffectParams p;
    p.control_points.push_back({0.3f, 0.3f, 0.4f, 0.4f});
    p.control_points.push_back({0.7f, 0.7f, 0.6f, 0.6f});
    REQUIRE(me::compose::apply_warp_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_warp_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(a == b);
}
