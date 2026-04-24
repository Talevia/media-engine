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

}  // namespace me::text
