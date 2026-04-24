#include "text/subtitle_renderer.hpp"

#include <ass/ass.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace me::text {

namespace {

/* Alpha-composite one libass ASS_Image onto a destination RGBA8
 * buffer. libass emits a chain of alpha-only bitmaps + a uniform
 * RGBA color per image; we iterate pixels, mix with src-over. */
void composite_image(const ASS_Image*  img,
                     std::uint8_t*     dst_rgba,
                     int               dst_w,
                     int               dst_h,
                     std::size_t       stride_bytes) {
    const std::uint32_t color = img->color;
    /* libass color is 0xRRGGBBAA where AA is transparency
     * (0 = opaque, 255 = transparent). Convert to our alpha
     * (0 = transparent, 255 = opaque). */
    const std::uint8_t  r_u8 = static_cast<std::uint8_t>((color >> 24) & 0xFF);
    const std::uint8_t  g_u8 = static_cast<std::uint8_t>((color >> 16) & 0xFF);
    const std::uint8_t  b_u8 = static_cast<std::uint8_t>((color >>  8) & 0xFF);
    const std::uint8_t  inv_a = static_cast<std::uint8_t>(color & 0xFF);
    const std::uint8_t  base_alpha = static_cast<std::uint8_t>(255 - inv_a);

    const int x0 = std::max(0, img->dst_x);
    const int y0 = std::max(0, img->dst_y);
    const int x1 = std::min(dst_w, img->dst_x + img->w);
    const int y1 = std::min(dst_h, img->dst_y + img->h);

    for (int y = y0; y < y1; ++y) {
        const std::uint8_t* src_row =
            img->bitmap + (y - img->dst_y) * img->stride;
        std::uint8_t* dst_row =
            dst_rgba + y * stride_bytes;
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t src_a = src_row[x - img->dst_x];
            if (src_a == 0) continue;
            /* Effective glyph alpha = base_alpha * src_a / 255. */
            const std::uint16_t eff_a =
                (static_cast<std::uint16_t>(base_alpha) *
                 static_cast<std::uint16_t>(src_a)) / 255;
            if (eff_a == 0) continue;

            std::uint8_t* p = dst_row + x * 4;
            const std::uint16_t inv = 255 - eff_a;
            /* src-over: dst = src.rgb*eff_a/255 + dst.rgb*(1-eff_a/255). */
            p[0] = static_cast<std::uint8_t>(
                (r_u8 * eff_a + p[0] * inv) / 255);
            p[1] = static_cast<std::uint8_t>(
                (g_u8 * eff_a + p[1] * inv) / 255);
            p[2] = static_cast<std::uint8_t>(
                (b_u8 * eff_a + p[2] * inv) / 255);
            /* dst alpha: combine existing + new glyph. */
            const std::uint16_t combined = eff_a + (p[3] * inv) / 255;
            p[3] = static_cast<std::uint8_t>(std::min<std::uint16_t>(combined, 255));
        }
    }
}

}  // namespace

SubtitleRenderer::SubtitleRenderer(int width, int height)
    : width_(width), height_(height) {
    library_ = ass_library_init();
    if (!library_) return;

    renderer_ = ass_renderer_init(library_);
    if (!renderer_) {
        ass_library_done(library_);
        library_ = nullptr;
        return;
    }

    ass_set_frame_size(renderer_, width_, height_);
    ass_set_storage_size(renderer_, width_, height_);

    /* `ass_set_fonts(renderer, defaults=NULL, default_family="Arial",
     * defaults=ASS_FONTPROVIDER_AUTODETECT, config=NULL,
     * update=true)` — lets libass pick a platform-appropriate font
     * provider (CoreText on macOS, fontconfig on Linux). */
    ass_set_fonts(renderer_,
                  /*default_font=*/nullptr,
                  /*default_family=*/"Arial",
                  /*default_fontprovider=*/ASS_FONTPROVIDER_AUTODETECT,
                  /*config=*/nullptr,
                  /*update=*/1);
}

SubtitleRenderer::~SubtitleRenderer() {
    if (track_)    ass_free_track(track_);
    if (renderer_) ass_renderer_done(renderer_);
    if (library_)  ass_library_done(library_);
}

bool SubtitleRenderer::load_from_memory(std::string_view content,
                                         const char* codepage) {
    if (!library_ || !renderer_) return false;
    if (track_) {
        ass_free_track(track_);
        track_ = nullptr;
    }
    /* ass_read_memory takes a non-const char* (historical libass
     * API). `content`'s data is not modified; the const_cast is
     * safe for the current libass version per its docs.
     *
     * codepage == nullptr → libass assumes UTF-8. */
    track_ = ass_read_memory(library_,
                              const_cast<char*>(content.data()),
                              content.size(),
                              codepage ? const_cast<char*>(codepage) : nullptr);
    valid_ = (track_ != nullptr);
    return valid_;
}

void SubtitleRenderer::render_frame(int64_t         t_ms,
                                     std::uint8_t*   out_rgba,
                                     std::size_t     stride_bytes) {
    if (!valid_ || !renderer_ || !track_ || !out_rgba) return;

    int changed = 0;
    ASS_Image* img = ass_render_frame(renderer_, track_, t_ms, &changed);

    /* ass_render_frame returns a linked list of ASS_Image bitmaps;
     * each represents one glyph quad with its own color + alpha
     * mask. Composite them all over the dst buffer in order
     * (libass orders the chain back-to-front). */
    for (; img != nullptr; img = img->next) {
        composite_image(img, out_rgba, width_, height_, stride_bytes);
    }
}

}  // namespace me::text
