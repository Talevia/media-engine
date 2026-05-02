/* test_posterize_kernel_pixel — M12 §156 (4/5). */
#include <doctest/doctest.h>

#include "compose/posterize_kernel.hpp"

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

TEST_CASE("apply_posterize_inplace: null buffer rejected") {
    me::PosterizeEffectParams p; p.levels = 4;
    CHECK(me::compose::apply_posterize_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_posterize_inplace: out-of-range levels rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::PosterizeEffectParams p;
    p.levels = 1;
    CHECK(me::compose::apply_posterize_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.levels = 257;
    CHECK(me::compose::apply_posterize_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_posterize_inplace: levels=256 → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::PosterizeEffectParams p;  /* defaults: levels=256 */
    REQUIRE(me::compose::apply_posterize_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_posterize_inplace: levels=2 → binary per channel") {
    /* Build a 6x1 image with R = 0, 50, 100, 150, 200, 255. With
     * levels=2 each channel maps to 0 or 255: bucket =
     * (in*1+127)/255 ∈ {0, 1}; out = bucket*255. */
    std::vector<std::uint8_t> buf(6 * 4);
    const std::uint8_t inputs[6] = {0, 50, 100, 150, 200, 255};
    for (int x = 0; x < 6; ++x) {
        buf[x * 4 + 0] = inputs[x];
        buf[x * 4 + 1] = 0;
        buf[x * 4 + 2] = 0;
        buf[x * 4 + 3] = 255;
    }
    me::PosterizeEffectParams p;
    p.levels = 2;
    REQUIRE(me::compose::apply_posterize_inplace(buf.data(), 6, 1, 24, p)
            == ME_OK);

    /* bucket = (in + 127) / 255: 0..127 → 0, 128..255 → 1.
     * So R values: 0→0, 50→0, 100→0, 150→255, 200→255, 255→255. */
    CHECK(buf[0 * 4 + 0] == 0);
    CHECK(buf[1 * 4 + 0] == 0);
    CHECK(buf[2 * 4 + 0] == 0);
    CHECK(buf[3 * 4 + 0] == 255);
    CHECK(buf[4 * 4 + 0] == 255);
    CHECK(buf[5 * 4 + 0] == 255);
}

TEST_CASE("apply_posterize_inplace: levels=4 → 4-step quantization") {
    /* 4 levels → bucket boundaries at multiples of 255/3 ≈ 85.
     * Bucket-representative values: 0, 85, 170, 255. */
    std::vector<std::uint8_t> buf(8 * 4);
    const std::uint8_t inputs[8] = {0, 32, 64, 96, 128, 160, 192, 255};
    for (int x = 0; x < 8; ++x) {
        buf[x * 4 + 0] = inputs[x];
        buf[x * 4 + 1] = 0;
        buf[x * 4 + 2] = 0;
        buf[x * 4 + 3] = 255;
    }
    me::PosterizeEffectParams p;
    p.levels = 4;
    REQUIRE(me::compose::apply_posterize_inplace(buf.data(), 8, 1, 32, p)
            == ME_OK);

    /* Each output should be one of {0, 85, 170, 255}. */
    for (int x = 0; x < 8; ++x) {
        const std::uint8_t r = buf[x * 4 + 0];
        const bool ok = (r == 0 || r == 85 || r == 170 || r == 255);
        CHECK(ok);
    }
}

TEST_CASE("apply_posterize_inplace: alpha never modified") {
    auto buf = make_solid(4, 4, 16, 100, 100, 100, 77);
    me::PosterizeEffectParams p;
    p.levels = 4;
    REQUIRE(me::compose::apply_posterize_inplace(buf.data(), 4, 4, 16, p)
            == ME_OK);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            CHECK(buf[(y * 16) + x * 4 + 3] == 77);
        }
    }
}

TEST_CASE("apply_posterize_inplace: determinism") {
    auto a = make_solid(16, 16, 64, 200, 100, 50, 255);
    auto b = make_solid(16, 16, 64, 200, 100, 50, 255);
    me::PosterizeEffectParams p;
    p.levels = 6;
    REQUIRE(me::compose::apply_posterize_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_posterize_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(a == b);
}
