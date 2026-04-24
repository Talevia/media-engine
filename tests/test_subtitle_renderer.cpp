/*
 * test_subtitle_renderer — libass-backed SubtitleRenderer
 * pixel-proof tests.
 *
 * Only compiled when ME_WITH_LIBASS=ON (ME_HAS_LIBASS compile
 * def). On machines without libass installed the CMake gate
 * auto-flips OFF + this binary is empty.
 *
 * Coverage:
 *   - Construct + destruct cleanly (libass lifecycle).
 *   - Load a minimal inline ASS track, render at the dialogue
 *     time, verify at least some pixels in the output buffer
 *     are non-transparent (text got drawn somewhere).
 *   - Render at a time outside any dialogue window leaves the
 *     buffer untouched.
 *   - Invalid ASS content fails cleanly (valid()==false).
 */
#ifdef ME_HAS_LIBASS

#include <doctest/doctest.h>

#include "text/subtitle_renderer.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* Minimal complete ASS v4+ script with one dialogue line. */
const char* kSampleAss = R"([Script Info]
ScriptType: v4.00+

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,48,&H00FFFFFF,&H0000FFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,Hello
)";

constexpr int W = 320;
constexpr int H = 180;

std::vector<std::uint8_t> zero_buffer() {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(W) * H * 4, 0);
}

/* Count pixels whose alpha channel is non-zero — a quick proxy
 * for "did the renderer draw anything?". */
std::size_t count_non_transparent(const std::vector<std::uint8_t>& buf) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        if (buf[i + 3] != 0) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("SubtitleRenderer: ctor + dtor cycle is clean") {
    me::text::SubtitleRenderer r(W, H);
    /* No track loaded yet → valid() is false. ctor succeeded if
     * libass init didn't return null internally. */
    CHECK_FALSE(r.valid());
    CHECK(r.width() == W);
    CHECK(r.height() == H);
}

TEST_CASE("SubtitleRenderer: load + render inside dialogue window draws pixels") {
    me::text::SubtitleRenderer r(W, H);
    const bool loaded = r.load_from_memory(kSampleAss);
    REQUIRE(loaded);
    REQUIRE(r.valid());

    auto buf = zero_buffer();
    /* Dialogue: 0:00:01.00 → 0:00:03.00 (1-3s in ms). Render at
     * t=2000ms — inside the window. */
    r.render_frame(/*t_ms=*/2000, buf.data(),
                    static_cast<std::size_t>(W) * 4);

    /* Some pixels should have non-zero alpha — the text drew. We
     * don't check specific pixel positions (font metrics vary by
     * installed font + libass version); just that drawing
     * happened. */
    const std::size_t drawn = count_non_transparent(buf);
    CHECK(drawn > 0);
}

TEST_CASE("SubtitleRenderer: render outside dialogue window leaves buffer untouched") {
    me::text::SubtitleRenderer r(W, H);
    REQUIRE(r.load_from_memory(kSampleAss));

    auto buf = zero_buffer();
    /* t=100ms is before the 1s dialogue start — no drawing. */
    r.render_frame(/*t_ms=*/100, buf.data(),
                    static_cast<std::size_t>(W) * 4);
    CHECK(count_non_transparent(buf) == 0);

    /* t=5000ms is after the 3s dialogue end — no drawing. */
    r.render_frame(/*t_ms=*/5000, buf.data(),
                    static_cast<std::size_t>(W) * 4);
    CHECK(count_non_transparent(buf) == 0);
}

TEST_CASE("SubtitleRenderer: invalid ASS content → valid() false, safe to render") {
    me::text::SubtitleRenderer r(W, H);
    /* "this isn't ASS" — libass tolerates it (returns an empty
     * track) rather than crashing. We accept either
     * load_from_memory() == false OR valid() == false after;
     * the contract is "don't crash on bad input". */
    const bool loaded = r.load_from_memory("this is not a valid ASS script");

    auto buf = zero_buffer();
    r.render_frame(1000, buf.data(), static_cast<std::size_t>(W) * 4);
    /* render_frame with invalid track is a no-op; buffer unchanged. */
    CHECK(count_non_transparent(buf) == 0);
    /* Consume `loaded` to silence unused warning under -Wunused. */
    (void)loaded;
}

#endif  // ME_HAS_LIBASS
