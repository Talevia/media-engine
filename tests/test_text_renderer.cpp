/*
 * test_text_renderer — pixel-proof tests for
 * me::text::TextRenderer (TextClipParams → RGBA8 canvas).
 *
 * ME_WITH_SKIA-only (Skia is the render backend).
 *
 * Coverage:
 *   - Static params render text to the canvas.
 *   - Animated font_size produces different pixel counts at
 *     different times (small t → small glyphs → few pixels;
 *     large t → large glyphs → more pixels).
 *   - Animated x/y moves glyphs around (pixels shift with the
 *     position).
 *   - Color parsing round-trips #RRGGBB + #RRGGBBAA.
 */
#ifdef ME_HAS_SKIA

#include <doctest/doctest.h>

#include "text/text_renderer.hpp"
#include "timeline/animated_number.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstdint>
#include <vector>

namespace {

constexpr int W = 256;
constexpr int H = 64;

std::vector<std::uint8_t> zero_buf() {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(W) * H * 4, 0);
}

std::size_t count_non_transparent(const std::vector<std::uint8_t>& v) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < v.size(); i += 4) {
        if (v[i + 3] != 0) ++n;
    }
    return n;
}

/* Find the centroid of non-transparent pixels (x_sum/n, y_sum/n).
 * Useful to verify position shifts. Returns {-1,-1} on empty. */
struct Centroid { double x = -1.0; double y = -1.0; };
Centroid centroid(const std::vector<std::uint8_t>& v) {
    double sx = 0.0, sy = 0.0;
    std::size_t n = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            if (v[i + 3] != 0) {
                sx += x;
                sy += y;
                ++n;
            }
        }
    }
    if (n == 0) return {};
    return { sx / n, sy / n };
}

}  // namespace

TEST_CASE("TextRenderer: static params render non-transparent pixels") {
    me::text::TextRenderer r(W, H);
    REQUIRE(r.valid());

    me::TextClipParams p;
    p.content    = "Hi";
    p.color      = me::AnimatedColor::from_hex("#FFFFFF");  // white
    p.font_size  = me::AnimatedNumber::from_static(40.0);
    p.x          = me::AnimatedNumber::from_static(16.0);
    p.y          = me::AnimatedNumber::from_static(48.0);

    auto buf = zero_buf();
    r.render(p, me_rational_t{0, 1}, buf.data(),
             static_cast<std::size_t>(W) * 4);
    CHECK(count_non_transparent(buf) > 0);
}

TEST_CASE("TextRenderer: animated font_size → larger pixel coverage") {
    me::text::TextRenderer r(W, H);
    REQUIRE(r.valid());

    /* Keyframed font_size: 16px at t=0, 48px at t=1s. */
    std::vector<me::Keyframe> kfs{
        {me_rational_t{0, 1}, 16.0,
         me::Interp::Linear, {}},
        {me_rational_t{1, 1}, 48.0,
         me::Interp::Linear, {}},
    };

    me::TextClipParams p;
    p.content   = "X";
    p.color     = me::AnimatedColor::from_hex("#FFFFFF");
    p.font_size = me::AnimatedNumber::from_keyframes(std::move(kfs));
    p.x         = me::AnimatedNumber::from_static(16.0);
    p.y         = me::AnimatedNumber::from_static(48.0);

    auto small = zero_buf();
    r.render(p, me_rational_t{0, 1}, small.data(),
             static_cast<std::size_t>(W) * 4);
    const std::size_t small_n = count_non_transparent(small);

    auto big = zero_buf();
    r.render(p, me_rational_t{1, 1}, big.data(),
             static_cast<std::size_t>(W) * 4);
    const std::size_t big_n = count_non_transparent(big);

    /* 48px vs 16px glyph should produce roughly 3× area. Allow
     * slack for antialias halo — just assert "strictly more". */
    CHECK(big_n > small_n);
    CHECK(big_n > small_n * 2);  // firm expectation: ≥2x
}

TEST_CASE("TextRenderer: animated x shifts centroid right over time") {
    me::text::TextRenderer r(W, H);
    REQUIRE(r.valid());

    std::vector<me::Keyframe> kfs{
        {me_rational_t{0, 1}, 16.0,  me::Interp::Linear, {}},
        {me_rational_t{1, 1}, 160.0, me::Interp::Linear, {}},
    };

    me::TextClipParams p;
    p.content   = "dot";
    p.color     = me::AnimatedColor::from_hex("#FFFFFF");
    p.font_size = me::AnimatedNumber::from_static(32.0);
    p.x         = me::AnimatedNumber::from_keyframes(std::move(kfs));
    p.y         = me::AnimatedNumber::from_static(48.0);

    auto left = zero_buf();
    r.render(p, me_rational_t{0, 1}, left.data(),
             static_cast<std::size_t>(W) * 4);
    const auto c_left = centroid(left);

    auto right = zero_buf();
    r.render(p, me_rational_t{1, 1}, right.data(),
             static_cast<std::size_t>(W) * 4);
    const auto c_right = centroid(right);

    REQUIRE(c_left.x > 0);
    REQUIRE(c_right.x > 0);
    /* Moved ~144px right → centroid should shift substantially. */
    CHECK(c_right.x > c_left.x + 50.0);
}

TEST_CASE("TextRenderer: max_width wraps long content across multiple lines") {
    /* Pin text-clip-multiline-word-wrap: when TextClipParams has
     * max_width set, TextRenderer routes through
     * SkiaBackend::draw_paragraph which greedy-wraps at
     * codepoint boundaries. A long CJK string at a small
     * max_width must fill multiple y-bands; the same string
     * without max_width should produce a single band that
     * extends beyond the canvas right edge (pixels at the
     * overflow x don't fit in W=256). */
    const int tall_h = 192;
    me::text::TextRenderer r(W, tall_h);
    REQUIRE(r.valid());

    me::TextClipParams p;
    /* ~20 CJK + emoji codepoints — well over W=256 at 32px
     * glyph width. */
    p.content   = "你好世界日本語中国語test english 🎉 emoji";
    p.color     = me::AnimatedColor::from_hex("#FFFFFF");
    p.font_size = me::AnimatedNumber::from_static(32.0);
    p.x         = me::AnimatedNumber::from_static(4.0);
    p.y         = me::AnimatedNumber::from_static(40.0);
    p.max_width = 240.0;  /* fits inside the W=256 canvas */
    p.line_height_multiplier = 1.4;

    /* Re-use the test's H=tall_h: pixel-scan the output for
     * non-transparent y-rows; multi-line output should touch
     * at least two separated y-bands (baselines at
     * 40 / 40+32*1.4 = 44.8 → 84). */
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(W) * tall_h * 4, 0);
    r.render(p, me_rational_t{0, 1}, buf.data(),
             static_cast<std::size_t>(W) * 4);

    /* Count rows with any non-transparent pixel; multi-line
     * output crosses at least two distinct y-bands. */
    int rows_with_ink = 0;
    for (int y = 0; y < tall_h; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            if (buf[i + 3] != 0) { ++rows_with_ink; break; }
        }
    }
    /* A single 32px line usually paints ~25-40 rows of ink.
     * Two lines should paint ≥45. Use a conservative floor
     * to keep the test stable across font versions. */
    CHECK(rows_with_ink > 40);

    /* Sanity: y-band separation. Find a y somewhere between
     * the first-line baseline (~40) and second-line baseline
     * (~85) that has no ink — proves two lines, not just one
     * big line. */
    auto any_ink_at_row = [&](int y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            if (buf[i + 3] != 0) return true;
        }
        return false;
    };
    bool found_gap = false;
    for (int y = 50; y < 75; ++y) {
        if (!any_ink_at_row(y)) { found_gap = true; break; }
    }
    CHECK(found_gap);
}

TEST_CASE("TextRenderer: explicit newline forces a break even inside max_width") {
    me::text::TextRenderer r(W, 128);
    REQUIRE(r.valid());

    me::TextClipParams p;
    p.content   = "A\nB";    /* two short lines, both fit max_width */
    p.color     = me::AnimatedColor::from_hex("#FFFFFF");
    p.font_size = me::AnimatedNumber::from_static(24.0);
    p.x         = me::AnimatedNumber::from_static(4.0);
    p.y         = me::AnimatedNumber::from_static(30.0);
    p.max_width = 200.0;
    p.line_height_multiplier = 1.5;

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(W) * 128 * 4, 0);
    r.render(p, me_rational_t{0, 1}, buf.data(),
             static_cast<std::size_t>(W) * 4);

    /* Two distinct y-bands expected. Find the topmost inked row
     * and the bottommost inked row; separation ≥ line_height. */
    int top_y = -1, bot_y = -1;
    for (int y = 0; y < 128; ++y) {
        bool ink = false;
        for (int x = 0; x < W; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            if (buf[i + 3] != 0) { ink = true; break; }
        }
        if (ink) {
            if (top_y < 0) top_y = y;
            bot_y = y;
        }
    }
    REQUIRE(top_y >= 0);
    REQUIRE(bot_y >= 0);
    /* line_height = 24 * 1.5 = 36; two lines span ~36px between
     * baselines. Glyph ink extents are 15-20px; gap between
     * lines ≥ 10px in practice. */
    CHECK(bot_y - top_y >= 20);
}

TEST_CASE("TextRenderer: parse_hex_rgba round-trips #RRGGBB + #RRGGBBAA") {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;

    me::text::TextRenderer::parse_hex_rgba("#FF8040", r, g, b, a);
    CHECK(r == 0xFF);
    CHECK(g == 0x80);
    CHECK(b == 0x40);
    CHECK(a == 0xFF);  // default opaque

    me::text::TextRenderer::parse_hex_rgba("#11223380", r, g, b, a);
    CHECK(r == 0x11);
    CHECK(g == 0x22);
    CHECK(b == 0x33);
    CHECK(a == 0x80);

    /* Invalid shape → defaults to opaque white. */
    me::text::TextRenderer::parse_hex_rgba("bogus", r, g, b, a);
    CHECK(r == 0xFF);
    CHECK(g == 0xFF);
    CHECK(b == 0xFF);
    CHECK(a == 0xFF);
}

#endif  // ME_HAS_SKIA
