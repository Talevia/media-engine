/*
 * face_mosaic_kernel — full impl. See header for the pre-resolved-
 * bboxes contract.
 */
#include "compose/face_mosaic_kernel.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace me::compose {

namespace {

/* Clamp the bbox to the image extent. Returns true and writes the
 * clamped corners into out_* if the result is non-empty. */
bool clamp_bbox(const Bbox& bb, int img_w, int img_h,
                int* out_x0, int* out_y0, int* out_x1, int* out_y1) {
    const int x0 = std::max(0, static_cast<int>(bb.x0));
    const int y0 = std::max(0, static_cast<int>(bb.y0));
    const int x1 = std::min(img_w, static_cast<int>(bb.x1));
    const int y1 = std::min(img_h, static_cast<int>(bb.y1));
    if (x1 <= x0 || y1 <= y0) return false;
    *out_x0 = x0; *out_y0 = y0; *out_x1 = x1; *out_y1 = y1;
    return true;
}

/* Pixelate: walk the clamped bbox in `block_size_px` tiles, write
 * each pixel = per-channel mean over its tile (RGB only; alpha
 * preserved per-pixel since opacity isn't a privacy axis). Tiles
 * at the right / bottom edge of the bbox are clipped to whatever
 * remains; the mean is over the actual tile size, not the nominal
 * block_size. */
void apply_pixelate_in_bbox(
    std::uint8_t* rgba, int img_w, int /*img_h*/, std::size_t stride,
    int x0, int y0, int x1, int y1, int block_size_px) {

    for (int by = y0; by < y1; by += block_size_px) {
        const int by_end = std::min(by + block_size_px, y1);
        for (int bx = x0; bx < x1; bx += block_size_px) {
            const int bx_end = std::min(bx + block_size_px, x1);

            const int tile_w = bx_end - bx;
            const int tile_h = by_end - by;
            const std::uint32_t tile_count =
                static_cast<std::uint32_t>(tile_w) *
                static_cast<std::uint32_t>(tile_h);

            /* Sum per-channel (R, G, B) over the tile. uint32 holds
             * up to 256 * 256 * 255 = 16 711 680 — far inside uint32
             * range for any sane block_size. */
            std::uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
            for (int y = by; y < by_end; ++y) {
                const std::uint8_t* row =
                    rgba + static_cast<std::size_t>(y) * stride;
                for (int x = bx; x < bx_end; ++x) {
                    const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
                    sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
                }
            }
            /* Round-half-up integer mean: (sum + N/2) / N. */
            const std::uint32_t half = tile_count / 2;
            const std::uint8_t mean_r =
                static_cast<std::uint8_t>((sum_r + half) / tile_count);
            const std::uint8_t mean_g =
                static_cast<std::uint8_t>((sum_g + half) / tile_count);
            const std::uint8_t mean_b =
                static_cast<std::uint8_t>((sum_b + half) / tile_count);

            for (int y = by; y < by_end; ++y) {
                std::uint8_t* row =
                    rgba + static_cast<std::size_t>(y) * stride;
                for (int x = bx; x < bx_end; ++x) {
                    std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
                    px[0] = mean_r; px[1] = mean_g; px[2] = mean_b;
                    /* px[3] (alpha) intentionally unchanged. */
                }
            }
        }
    }
    (void)img_w;  /* clamp_bbox already validated. */
}

/* Blur: box-filter at radius `block_size_px / 2`. Two-pass
 * (horizontal then vertical) for O(N * R) instead of O(N * R²)
 * naive. The blur reads only inside the bbox extent (clamped at
 * bbox edges, no border replicate beyond the bbox), so the
 * privacy mask doesn't bleed into surrounding pixels. RGBA buffer
 * is in-place per-row using a tiny per-row scratch pattern: for
 * a strict in-place we'd need a full bbox-sized scratch since
 * the output of pass 1 feeds pass 2. Use a heap scratch of
 * (bbox_w × bbox_h × 4) RGBA bytes — small relative to a full
 * frame (face bbox is typically ≤ 256² = 256 KB). */
void apply_blur_in_bbox(
    std::uint8_t* rgba, int /*img_w*/, int /*img_h*/, std::size_t stride,
    int x0, int y0, int x1, int y1, int block_size_px) {

    const int radius = block_size_px / 2;
    if (radius <= 0) return;

    const int bw = x1 - x0;
    const int bh = y1 - y0;

    /* Two scratch buffers: one for horizontal pass output, one
     * for the final vertical pass output. */
    const std::size_t scratch_bytes =
        static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * 4;
    std::vector<std::uint8_t> tmp(scratch_bytes);

    /* Horizontal pass: tmp[y][x] = mean of rgba[y][x-radius .. x+radius]
     * clamped to [x0, x1). */
    for (int y = 0; y < bh; ++y) {
        const std::uint8_t* src_row =
            rgba + static_cast<std::size_t>(y0 + y) * stride;
        std::uint8_t* dst_row = tmp.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(bw) * 4;

        for (int x = 0; x < bw; ++x) {
            const int lo = std::max(0,      x - radius);
            const int hi = std::min(bw - 1, x + radius);
            const int n  = hi - lo + 1;

            std::uint32_t sr = 0, sg = 0, sb = 0, sa = 0;
            for (int xi = lo; xi <= hi; ++xi) {
                const std::uint8_t* px =
                    src_row + static_cast<std::size_t>(x0 + xi) * 4;
                sr += px[0]; sg += px[1]; sb += px[2]; sa += px[3];
            }
            const std::uint32_t half = static_cast<std::uint32_t>(n) / 2;
            std::uint8_t* dst = dst_row + static_cast<std::size_t>(x) * 4;
            dst[0] = static_cast<std::uint8_t>((sr + half) / n);
            dst[1] = static_cast<std::uint8_t>((sg + half) / n);
            dst[2] = static_cast<std::uint8_t>((sb + half) / n);
            dst[3] = static_cast<std::uint8_t>((sa + half) / n);
        }
    }

    /* Vertical pass: write back into rgba bbox region. */
    for (int x = 0; x < bw; ++x) {
        for (int y = 0; y < bh; ++y) {
            const int lo = std::max(0,      y - radius);
            const int hi = std::min(bh - 1, y + radius);
            const int n  = hi - lo + 1;

            std::uint32_t sr = 0, sg = 0, sb = 0, sa = 0;
            for (int yi = lo; yi <= hi; ++yi) {
                const std::uint8_t* px = tmp.data() +
                    (static_cast<std::size_t>(yi) *
                     static_cast<std::size_t>(bw) +
                     static_cast<std::size_t>(x)) * 4;
                sr += px[0]; sg += px[1]; sb += px[2]; sa += px[3];
            }
            const std::uint32_t half = static_cast<std::uint32_t>(n) / 2;
            std::uint8_t* dst =
                rgba + static_cast<std::size_t>(y0 + y) * stride +
                static_cast<std::size_t>(x0 + x) * 4;
            dst[0] = static_cast<std::uint8_t>((sr + half) / n);
            dst[1] = static_cast<std::uint8_t>((sg + half) / n);
            dst[2] = static_cast<std::uint8_t>((sb + half) / n);
            dst[3] = static_cast<std::uint8_t>((sa + half) / n);
        }
    }
}

}  // namespace

me_status_t apply_face_mosaic_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceMosaicEffectParams&      params,
    std::span<const Bbox>                  landmark_bboxes) {

    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (params.block_size_px <= 0) return ME_E_INVALID_ARG;

    /* Empty bboxes → no-op (resolver may emit empty for low-
     * confidence frames). */
    if (landmark_bboxes.empty()) return ME_OK;

    for (const Bbox& bb : landmark_bboxes) {
        if (bb.empty()) continue;
        int x0, y0, x1, y1;
        if (!clamp_bbox(bb, width, height, &x0, &y0, &x1, &y1)) continue;

        switch (params.kind) {
        case me::FaceMosaicEffectParams::Kind::Pixelate:
            apply_pixelate_in_bbox(rgba, width, height, stride_bytes,
                                    x0, y0, x1, y1, params.block_size_px);
            break;
        case me::FaceMosaicEffectParams::Kind::Blur:
            apply_blur_in_bbox(rgba, width, height, stride_bytes,
                                x0, y0, x1, y1, params.block_size_px);
            break;
        }
    }
    return ME_OK;
}

}  // namespace me::compose
