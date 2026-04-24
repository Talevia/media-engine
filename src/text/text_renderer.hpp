/*
 * me::text::TextRenderer — TextClipParams → RGBA8 canvas
 * rasterizer.
 *
 * M5 exit criterion "Text clip (静态 + 动画字号 / 颜色 / 位置)"
 * render half. Consumes a `me::TextClipParams` (from
 * timeline_impl.hpp, loaded by text-clip-ir cycle 16) and a
 * timeline-global time `t`, evaluates each animated field at
 * `t`, then draws via SkiaBackend onto a caller-supplied RGBA8
 * canvas buffer.
 *
 * Ownership: TextRenderer owns its SkiaBackend instance,
 * which in turn owns the Skia surface + font manager. Create
 * one per clip when you enter the clip's time range; destroy
 * when exiting (Skia ctors are cheap — shader programs + font
 * lookups get cached internally, but the surface alloc is a
 * malloc-ish cost).
 *
 * Compiled only when `-DME_WITH_SKIA=ON` (ME_HAS_SKIA
 * compile def).
 *
 * Threading: not thread-safe; one renderer per rendering
 * thread.
 */
#pragma once

#include "timeline/timeline_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace me::text {

class SkiaBackend;

class TextRenderer {
public:
    /* Construct a renderer for `canvas_w × canvas_h` pixel RGBA8
     * output. The Skia surface matches those dimensions +
     * renders text with sRGB color space. */
    TextRenderer(int canvas_w, int canvas_h);
    ~TextRenderer();

    TextRenderer(const TextRenderer&)            = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    /* Render `params` at timeline time `t` onto `out_rgba`.
     * `stride_bytes` is the dst pitch (usually canvas_w × 4).
     * `out_rgba` is cleared to transparent before drawing;
     * the text is composited as the only content (caller
     * alpha-overs the result onto whatever underlying layer). */
    void render(const me::TextClipParams& params,
                me_rational_t             t,
                std::uint8_t*             out_rgba,
                std::size_t               stride_bytes);

    /* True iff the underlying SkiaBackend allocated successfully.
     * A false result means render() is a silent no-op. */
    bool valid() const noexcept;

    /* CSS-like hex color string ("#RRGGBB" / "#RRGGBBAA") → 4
     * bytes. Pure function; exposed public so unit tests can
     * pin the parser without indirecting through a full render.
     * Invalid input (bad shape / non-hex chars) → defaults to
     * opaque white. */
    static void parse_hex_rgba(const std::string& hex,
                                std::uint8_t& r, std::uint8_t& g,
                                std::uint8_t& b, std::uint8_t& a);

private:
    std::unique_ptr<SkiaBackend> backend_;
};

}  // namespace me::text
