/*
 * me::text::SkiaBackend — thin Skia wrapper for rasterizing
 * text + vector primitives onto an RGBA8 framebuffer.
 *
 * M5 exit criterion "Skia 集成" foundation. Skia provides a
 * mature 2D rasterizer that M5's text / future M8+ vector /
 * HDR work can build on. This first landing delivers just the
 * plumbing + a "draw a string" smoke path — enough to prove
 * the Skia .a links, SkCanvas initializes, and glyphs land in
 * our RGBA buffer. Richer surfaces (paragraph layout,
 * effects, path rendering) arrive with consumer bullets.
 *
 * Compiled only when `-DME_WITH_SKIA=ON`. Cross-platform
 * coverage today: macOS arm64 only — the JetBrains skia-pack
 * release ships per-platform .a bundles and we download only
 * the host platform's binary per the platform branch in
 * src/CMakeLists.txt. Adding Linux x64 / Windows is a
 * mechanical extension (parallel FetchContent_Declare +
 * IMPORTED_LOCATION branches).
 *
 * Opaque libass-style wrapping: the public header
 * (subtitle_renderer.hpp parallel) keeps Skia types off the
 * caller's include graph. The .cpp pulls in
 * <include/core/SkCanvas.h> etc. internally.
 *
 * Threading: Skia's SkSurface / SkCanvas are not thread-safe.
 * One SkiaBackend per thread, or external serialization.
 *
 * Ownership: ctor allocates an SkSurface at the given
 * dimensions + an associated SkCanvas. dtor releases both via
 * Skia's sk_sp<> reference counting.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace me::text {

class SkiaBackend {
public:
    /* Construct a pixel surface of `width × height` RGBA8. The
     * surface is rendered to by draw_* calls; read_pixels()
     * copies the current state into a caller buffer. */
    SkiaBackend(int width, int height);
    ~SkiaBackend();

    SkiaBackend(const SkiaBackend&)            = delete;
    SkiaBackend& operator=(const SkiaBackend&) = delete;

    /* Clear the surface to a solid RGBA color. */
    void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);

    /* Draw a UTF-8 string at (x, y) with the given font size +
     * solid RGBA fill color. y is the glyph baseline (Skia
     * convention — typical baselines land near the bottom of
     * a cap-height).
     *
     * Uses the platform's default font (auto-fallback by the
     * platform font-shaper covers most Unicode scripts — CoreText
     * on macOS handles CJK in the default run). Emoji + other
     * color-glyph Unicode ranges may render as tofu / blank
     * since `drawSimpleText` picks one typeface for the whole
     * string. For robust Unicode coverage use
     * `draw_string_with_fallback` which splits runs by codepoint
     * + picks a per-run typeface via matchFamilyStyleCharacter. */
    void draw_string(std::string_view text,
                     float            x,
                     float            y,
                     float            font_size,
                     std::uint8_t     r,
                     std::uint8_t     g,
                     std::uint8_t     b,
                     std::uint8_t     a);

    /* Like `draw_string` but walks codepoints + picks a typeface
     * for each run via `SkFontMgr::matchFamilyStyleCharacter`.
     * Handles:
     *   - CJK: CoreText's default run already covers this on
     *     macOS; explicit fallback is redundant but harmless.
     *   - Emoji: requires a color-glyph typeface (Apple Color
     *     Emoji on macOS / NotoColorEmoji on Linux). The default
     *     typeface doesn't have emoji glyphs — fallback picks
     *     the emoji face and renders those runs separately.
     *
     * Advances the x cursor by each run's measured width so
     * subsequent runs land beside the previous — crude but
     * correct for LTR scripts without bidi / complex shaping.
     * Full text layout (bidi, positioning, kerning across runs)
     * is a future SkShaper integration. */
    void draw_string_with_fallback(std::string_view text,
                                    float            x,
                                    float            y,
                                    float            font_size,
                                    std::uint8_t     r,
                                    std::uint8_t     g,
                                    std::uint8_t     b,
                                    std::uint8_t     a);

    /* Greedy word-wrap paragraph renderer. Breaks `text` into
     * lines where each line measures ≤ `max_width` pixels (as
     * computed by `SkFont::measureText`), then draws each line via
     * `draw_string_with_fallback` at a y offset advanced by
     * `font_size * line_height_multiplier` between lines.
     *
     * Break policy: explicit `\n` always starts a new line.
     * Otherwise the loop advances codepoint-by-codepoint; when
     * adding the next codepoint would push the current line past
     * `max_width`, it flushes whatever the line holds and starts
     * a fresh line with the pending codepoint. This gives
     * correct wrapping for CJK (every codepoint is a valid break
     * point) and for emoji, and acceptable wrapping for Latin
     * text (may split mid-word; smart whitespace-aware wrap is
     * a follow-up).
     *
     * When `max_width <= 0` or text is empty, degrades to a
     * single `draw_string_with_fallback` call. */
    void draw_paragraph(std::string_view text,
                        float            x,
                        float            y,
                        float            font_size,
                        float            max_width,
                        float            line_height_multiplier,
                        std::uint8_t     r,
                        std::uint8_t     g,
                        std::uint8_t     b,
                        std::uint8_t     a);

    /* Copy current RGBA8 surface contents to the caller's buffer.
     * `dst_rgba` must have at least `width × height × 4` bytes;
     * `stride_bytes` is the pitch (usually width * 4). Returns
     * true iff the read succeeded. */
    bool read_pixels(std::uint8_t* dst_rgba, std::size_t stride_bytes);

    /* True iff ctor successfully allocated the Skia surface.
     * draw_* / read_pixels are no-ops when false. */
    bool valid() const noexcept { return valid_; }

    int width()  const noexcept { return width_;  }
    int height() const noexcept { return height_; }

private:
    /* Opaque impl pointer — keeps Skia types out of the header.
     * Body is a struct in the .cpp holding sk_sp<SkSurface>. */
    struct Impl;
    Impl* impl_ = nullptr;

    int  width_  = 0;
    int  height_ = 0;
    bool valid_  = false;
};

}  // namespace me::text
