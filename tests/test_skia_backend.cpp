/*
 * test_skia_backend — pixel-proof tests for me::text::SkiaBackend.
 *
 * ME_WITH_SKIA-only; the test body is ME_HAS_SKIA-guarded so
 * builds without Skia produce a 0-case doctest binary that
 * still links and runs as a no-op.
 *
 * Coverage:
 *   - Ctor/dtor cycle is clean (Skia surface alloc / free).
 *   - clear() fills the buffer with the requested color.
 *   - draw_string() writes non-transparent pixels where text
 *     lands (proves font resolution + glyph rasterization
 *     actually happen, not just that the API calls don't
 *     crash).
 *   - read_pixels preserves RGBA channel order (clear to a
 *     known color + read back verifies).
 */
#ifdef ME_HAS_SKIA

#include <doctest/doctest.h>

#include "text/skia_backend.hpp"

#include <cstdint>
#include <vector>

namespace {

constexpr int W = 256;
constexpr int H = 64;

std::vector<std::uint8_t> zero_buf() {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(W) * H * 4, 0);
}

/* Count pixels whose alpha is non-zero — proxy for "text drew
 * something". */
std::size_t count_non_transparent(const std::vector<std::uint8_t>& v) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < v.size(); i += 4) {
        if (v[i + 3] != 0) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("SkiaBackend: ctor + dtor cycle succeeds") {
    me::text::SkiaBackend sk(W, H);
    CHECK(sk.valid());
    CHECK(sk.width() == W);
    CHECK(sk.height() == H);
}

TEST_CASE("SkiaBackend: clear + read_pixels round-trip preserves channel order") {
    me::text::SkiaBackend sk(W, H);
    REQUIRE(sk.valid());

    /* Clear to distinctive RGB value so a channel swap would
     * be obvious. */
    sk.clear(0x20, 0x40, 0x80, 0xFF);

    auto buf = zero_buf();
    REQUIRE(sk.read_pixels(buf.data(), static_cast<std::size_t>(W) * 4));

    /* Sample the center pixel; all pixels should be identical
     * for uniform clear. */
    const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
    /* Allow ±2 ULP for premul/unpremul rounding. */
    auto roughly = [](int v, int target) {
        return v >= target - 2 && v <= target + 2;
    };
    CHECK(roughly(buf[mid + 0], 0x20));  // R
    CHECK(roughly(buf[mid + 1], 0x40));  // G
    CHECK(roughly(buf[mid + 2], 0x80));  // B
    CHECK(buf[mid + 3] == 0xFF);
}

TEST_CASE("SkiaBackend: draw_string writes non-transparent pixels") {
    me::text::SkiaBackend sk(W, H);
    REQUIRE(sk.valid());

    /* Start with fully transparent surface so we can distinguish
     * "drew nothing" from "drew the clear color". */
    sk.clear(0x00, 0x00, 0x00, 0x00);
    sk.draw_string("Hello", /*x=*/16.0f, /*y=*/40.0f,
                   /*font_size=*/32.0f,
                   0xFF, 0xFF, 0xFF, 0xFF);

    auto buf = zero_buf();
    REQUIRE(sk.read_pixels(buf.data(), static_cast<std::size_t>(W) * 4));

    /* Font metrics + antialiasing vary by platform; just assert
     * some pixels landed. Typical "Hello" at 32pt should cover
     * at least a few hundred pixels. */
    const std::size_t drawn = count_non_transparent(buf);
    CHECK(drawn > 0);
}

TEST_CASE("SkiaBackend: empty text draws nothing") {
    me::text::SkiaBackend sk(W, H);
    REQUIRE(sk.valid());

    sk.clear(0x00, 0x00, 0x00, 0x00);
    sk.draw_string("", 16.0f, 40.0f, 32.0f, 0xFF, 0xFF, 0xFF, 0xFF);

    auto buf = zero_buf();
    REQUIRE(sk.read_pixels(buf.data(), static_cast<std::size_t>(W) * 4));

    CHECK(count_non_transparent(buf) == 0);
}

#endif  // ME_HAS_SKIA
