#include "text/skia_backend.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkString.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTextBlob.h>
#include <include/core/SkTypeface.h>
#include <include/ports/SkFontMgr_mac_ct.h>

#include <cstring>
#include <memory>

namespace me::text {

struct SkiaBackend::Impl {
    sk_sp<SkSurface>  surface;
    sk_sp<SkFontMgr>  font_mgr;
    sk_sp<SkTypeface> default_face;
};

SkiaBackend::SkiaBackend(int width, int height)
    : width_(width), height_(height) {
    impl_ = new Impl();

    /* N32Premul is Skia's native ARGB layout. We expose it as
     * RGBA8 via the read_pixels readback with an info-swap; Skia
     * handles the channel conversion. sRGB color space ensures
     * text renders with gamma-correct intermediate math. */
    const SkImageInfo info = SkImageInfo::Make(
        width_, height_,
        kRGBA_8888_SkColorType,
        kPremul_SkAlphaType,
        SkColorSpace::MakeSRGB());

    impl_->surface = SkSurfaces::Raster(info);
    if (!impl_->surface) return;

    /* Skia post-m85 requires an explicit SkFontMgr — the old
     * SkFontMgr::RefDefault() returned an empty singleton. On
     * macOS we wire CoreText, which auto-resolves to the system
     * font collection when passed nullptr. Linux / Windows
     * branches add SkFontMgr_New_FontConfig / _New_DirectWrite
     * in a later cycle. */
    impl_->font_mgr = SkFontMgr_New_CoreText(nullptr);
    if (!impl_->font_mgr) return;

    impl_->default_face = impl_->font_mgr->legacyMakeTypeface(
        /*familyName=*/nullptr, SkFontStyle::Normal());
    /* default_face may be null when no system fonts exist (unlikely
     * but possible in sandboxed environments); draw_string handles
     * that with a null-check + no-op. */

    valid_ = true;
}

SkiaBackend::~SkiaBackend() {
    delete impl_;
}

void SkiaBackend::clear(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (!valid_) return;
    SkCanvas* canvas = impl_->surface->getCanvas();
    canvas->clear(SkColorSetARGB(a, r, g, b));
}

void SkiaBackend::draw_string(std::string_view text,
                               float x, float y, float font_size,
                               std::uint8_t r, std::uint8_t g,
                               std::uint8_t b, std::uint8_t a) {
    if (!valid_ || text.empty()) return;
    SkCanvas* canvas = impl_->surface->getCanvas();

    /* Font — ctor's Impl::default_face is the system-default
     * typeface resolved via SkFontMgr_New_CoreText. Null face
     * (extremely sandboxed environments) short-circuits to a
     * silent no-op since SkFont with null sk_sp renders nothing
     * useful. */
    if (!impl_->default_face) return;
    SkFont font(impl_->default_face, font_size);

    SkPaint paint;
    paint.setColor(SkColorSetARGB(a, r, g, b));
    paint.setAntiAlias(true);

    canvas->drawSimpleText(text.data(), text.size(),
                           SkTextEncoding::kUTF8,
                           x, y, font, paint);
}

bool SkiaBackend::read_pixels(std::uint8_t* dst_rgba, std::size_t stride_bytes) {
    if (!valid_ || !dst_rgba) return false;
    const SkImageInfo info = SkImageInfo::Make(
        width_, height_,
        kRGBA_8888_SkColorType,
        kPremul_SkAlphaType,
        SkColorSpace::MakeSRGB());
    return impl_->surface->readPixels(info, dst_rgba, stride_bytes, 0, 0);
}

namespace {

/* Decode one UTF-8 codepoint from `p` (up to `end`). Return the
 * decoded SkUnichar (int32 Unicode scalar) and advance `p` past
 * the consumed bytes. On malformed input, emits U+FFFD
 * REPLACEMENT CHARACTER + advances one byte to make forward
 * progress. Used by draw_string_with_fallback to walk codepoints
 * for per-cp typeface selection. */
SkUnichar utf8_decode(const char*& p, const char* end) {
    if (p >= end) return 0;
    const unsigned char b0 = static_cast<unsigned char>(*p);
    int extra = 0;
    SkUnichar cp = 0;
    if      ((b0 & 0x80) == 0x00) { cp = b0;         extra = 0; }
    else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F;  extra = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F;  extra = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07;  extra = 3; }
    else { ++p; return 0xFFFD; }

    ++p;
    for (int i = 0; i < extra; ++i) {
        if (p >= end) return 0xFFFD;
        const unsigned char bn = static_cast<unsigned char>(*p);
        if ((bn & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (bn & 0x3F);
        ++p;
    }
    return cp;
}

}  // namespace

void SkiaBackend::draw_string_with_fallback(std::string_view text,
                                              float x, float y, float font_size,
                                              std::uint8_t r, std::uint8_t g,
                                              std::uint8_t b, std::uint8_t a) {
    if (!valid_ || text.empty()) return;
    if (!impl_->default_face || !impl_->font_mgr) return;

    SkCanvas* canvas = impl_->surface->getCanvas();
    SkPaint paint;
    paint.setColor(SkColorSetARGB(a, r, g, b));
    paint.setAntiAlias(true);

    const char* p   = text.data();
    const char* end = p + text.size();

    sk_sp<SkTypeface> run_face;
    std::string       run_text;
    float             cursor_x = x;

    auto flush_run = [&]() {
        if (run_text.empty() || !run_face) return;
        SkFont run_font(run_face, font_size);
        canvas->drawSimpleText(run_text.data(), run_text.size(),
                               SkTextEncoding::kUTF8,
                               cursor_x, y, run_font, paint);
        const SkScalar width = run_font.measureText(
            run_text.data(), run_text.size(),
            SkTextEncoding::kUTF8);
        cursor_x += width;
        run_text.clear();
    };

    while (p < end) {
        const char* cp_start = p;
        const SkUnichar cp = utf8_decode(p, end);
        if (cp == 0) break;

        /* Pick a typeface for this codepoint. Start with the
         * default face; if it lacks the glyph, use
         * matchFamilyStyleCharacter to find one that has it
         * (CoreText returns Apple Color Emoji for emoji codepoints
         * on macOS; the same API works with fontconfig on Linux
         * via the platform SkFontMgr variant). */
        sk_sp<SkTypeface> face = impl_->default_face;
        SkFont probe(face, font_size);
        if (probe.unicharToGlyph(cp) == 0) {
            sk_sp<SkTypeface> fallback = impl_->font_mgr->matchFamilyStyleCharacter(
                /*familyName=*/nullptr,
                SkFontStyle::Normal(),
                /*bcp47=*/nullptr, /*count=*/0,
                cp);
            if (fallback) face = fallback;
        }

        if (run_face.get() != face.get()) {
            flush_run();
            run_face = face;
        }
        run_text.append(cp_start, static_cast<std::size_t>(p - cp_start));
    }
    flush_run();
}

void SkiaBackend::draw_paragraph(std::string_view text,
                                   float x, float y, float font_size,
                                   float max_width, float line_height_mul,
                                   std::uint8_t r, std::uint8_t g,
                                   std::uint8_t b, std::uint8_t a) {
    if (!valid_ || text.empty()) return;
    if (max_width <= 0.0f || line_height_mul <= 0.0f) {
        draw_string_with_fallback(text, x, y, font_size, r, g, b, a);
        return;
    }
    if (!impl_->default_face) return;

    const SkFont probe(impl_->default_face, font_size);
    const float  line_height = font_size * line_height_mul;

    /* Wrap one paragraph slice [ps, pe) into lines; advance
     * `cursor_y` as it emits each line. Greedy per-codepoint:
     * keep extending the current line until the next codepoint
     * would push us past `max_width`, then flush and start
     * a new one. */
    float cursor_y = y;
    auto emit_paragraph = [&](const char* ps, const char* pe) {
        if (ps >= pe) {
            /* Empty paragraph from a lone `\n` — still advances
             * the cursor so explicit blank lines render. */
            cursor_y += line_height;
            return;
        }
        const char* line_start = ps;
        const char* line_end   = ps;
        while (line_end < pe) {
            /* Advance one codepoint past line_end to form a
             * candidate slice [line_start, next). */
            const char* next = line_end;
            (void)utf8_decode(next, pe);
            const std::string_view candidate(
                line_start, static_cast<std::size_t>(next - line_start));
            const SkScalar w = probe.measureText(
                candidate.data(), candidate.size(),
                SkTextEncoding::kUTF8);
            if (w > max_width && line_end > line_start) {
                /* Overflow: flush the current line, restart at
                 * the pending codepoint. */
                const std::string_view line(
                    line_start,
                    static_cast<std::size_t>(line_end - line_start));
                draw_string_with_fallback(line, x, cursor_y, font_size,
                                           r, g, b, a);
                cursor_y += line_height;
                line_start = line_end;
                /* Do not advance line_end — reprocess the pending
                 * codepoint as the first char of the new line. */
                continue;
            }
            line_end = next;
        }
        if (line_start < pe) {
            const std::string_view line(
                line_start,
                static_cast<std::size_t>(pe - line_start));
            draw_string_with_fallback(line, x, cursor_y, font_size,
                                       r, g, b, a);
            cursor_y += line_height;
        }
    };

    /* Split `text` on explicit `\n`. Each paragraph goes through
     * the wrapper; an explicit newline always forces a break. */
    const char* p          = text.data();
    const char* end        = p + text.size();
    const char* para_start = p;
    while (p < end) {
        if (*p == '\n') {
            emit_paragraph(para_start, p);
            para_start = p + 1;
        }
        ++p;
    }
    if (para_start <= end) {
        emit_paragraph(para_start, end);
    }
}

}  // namespace me::text
