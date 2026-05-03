/* test_displacement_kernel_pixel — M12 §158 (2/2). */
#include <doctest/doctest.h>

#include "compose/displacement_kernel.hpp"

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

/* Build a displacement texture of constant (R, G, 0, 255). */
std::vector<std::uint8_t> make_displacement_texture(int w, int h,
                                                       std::uint8_t r,
                                                       std::uint8_t g) {
    return make_solid(w, h, w * 4, r, g, 0, 255);
}

}  // namespace

TEST_CASE("apply_displacement_inplace: null buffer rejected") {
    auto tex = make_displacement_texture(4, 4, 128, 128);
    CHECK(me::compose::apply_displacement_inplace(
              nullptr, 16, 16, 64, tex.data(), 4, 4, 4.0f, 0.0f)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_displacement_inplace: strength=0 → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    auto tex = make_displacement_texture(4, 4, 200, 200);  /* would push if strength > 0 */
    REQUIRE(me::compose::apply_displacement_inplace(
                buf.data(), 8, 8, 32, tex.data(), 4, 4, 0.0f, 0.0f)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_displacement_inplace: neutral texture (R=G=128) → identity") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    auto tex = make_displacement_texture(4, 4, 128, 128);
    /* (128 * 2 - 255) = 1, so offset = (1 * 8) / 255 ≈ 0.031 px,
     * which rounds to 0 in the bilinear sampler. Output should
     * equal input bit-for-bit. */
    REQUIRE(me::compose::apply_displacement_inplace(
                buf.data(), 8, 8, 32, tex.data(), 4, 4, 8.0f, 8.0f)
            == ME_OK);
    /* Allow ±1 byte tolerance for bilinear-rounding noise from the
     * tiny residual offset. */
    for (std::size_t i = 0; i < buf.size(); ++i) {
        CHECK(std::abs(static_cast<int>(buf[i]) -
                        static_cast<int>(snapshot[i])) <= 1);
    }
}

TEST_CASE("apply_displacement_inplace: null texture with strength=0 ok") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    REQUIRE(me::compose::apply_displacement_inplace(
                buf.data(), 8, 8, 32, nullptr, 0, 0, 0.0f, 0.0f)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_displacement_inplace: null texture with strength!=0 rejected") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    CHECK(me::compose::apply_displacement_inplace(
              buf.data(), 8, 8, 32, nullptr, 0, 0, 4.0f, 0.0f)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_displacement_inplace: solid color preserved") {
    /* Uniform input + any displacement → output unchanged (every
     * sample reads the same color regardless of offset). */
    auto buf = make_solid(16, 16, 64, 73, 137, 211, 255);
    const auto snapshot = buf;
    auto tex = make_displacement_texture(8, 8, 200, 50);
    REQUIRE(me::compose::apply_displacement_inplace(
                buf.data(), 16, 16, 64, tex.data(), 8, 8, 8.0f, 8.0f)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_displacement_inplace: max-positive R shifts pixels right") {
    /* Input: vertical step (left half black, right half white).
     * Texture R = 255 everywhere → uniform offset_x = +strength.
     * Output should be input shifted left by `strength` (i.e.,
     * each output pixel reads from input at x+strength → output
     * looks like the right portion of input replicated).
     * Wait — clarification: source = output_pos + offset, so a
     * positive offset_x means "read further right" which makes
     * the output look as if shifted LEFT.
     *
     * Result: with strength_x = 4 and a step at column 8, the
     * step in the output shifts to column 4. */
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
    auto tex = make_displacement_texture(4, 4, 255, 128);
    REQUIRE(me::compose::apply_displacement_inplace(
                buf.data(), 16, 16, 64, tex.data(), 4, 4, 4.0f, 0.0f)
            == ME_OK);

    /* Output column 4 should read input column 8 (= white). */
    CHECK(buf[(8 * 16 + 4) * 4 + 0] >= 200);
    /* Output column 12 should still be white (input col 16 clamped). */
    CHECK(buf[(8 * 16 + 12) * 4 + 0] >= 200);
    /* Output column 0 should read input column 4 (= still black). */
    CHECK(buf[(8 * 16 + 0) * 4 + 0] <= 50);
}

TEST_CASE("apply_displacement_inplace: determinism") {
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
    auto tex = make_displacement_texture(8, 8, 180, 80);
    REQUIRE(me::compose::apply_displacement_inplace(
                a.data(), 16, 16, 64, tex.data(), 8, 8, 6.0f, 4.0f)
            == ME_OK);
    REQUIRE(me::compose::apply_displacement_inplace(
                b.data(), 16, 16, 64, tex.data(), 8, 8, 6.0f, 4.0f)
            == ME_OK);
    CHECK(a == b);
}
