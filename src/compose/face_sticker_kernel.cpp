/*
 * face_sticker_kernel — full impl. See header for the pre-
 * resolved-inputs contract.
 *
 * Path: for each non-empty bbox, compute (scale_x_eff, scale_y_eff)
 * = (bbox.width * params.scale_x / sticker.width, bbox.height *
 * params.scale_y / sticker.height) so the sticker sized to fit
 * the bbox at scale 1.0. Then translate so the sticker center
 * aligns with the bbox center plus (params.offset_x, offset_y).
 * Build the inverse affine via `compose_inverse_affine` (same
 * helper the M2 transform path uses) and call `affine_blit` into
 * a temp RGBA8 scratch the size of the bbox. Final step:
 * source-over alpha-blend the scratch onto the dst frame's bbox
 * region — the sticker's alpha channel is what gives the
 * pasted-on visual; opaque pixels overwrite, transparent pixels
 * leave the background untouched.
 */
#include "compose/face_sticker_kernel.hpp"

#include "compose/affine_blit.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace me::compose {

namespace {

/* Source-over blend (Porter-Duff over): `dst = src + dst * (1 -
 * src.a)`. Fixed-point math with /255 rounding so byte arithmetic
 * stays deterministic. */
inline void blend_over_pixel(std::uint8_t* dst, const std::uint8_t* src) {
    const std::uint32_t sa = src[3];
    if (sa == 0) return;                /* fully transparent — no op */
    if (sa == 255) {                    /* fully opaque — overwrite */
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        return;
    }
    const std::uint32_t inv_sa = 255 - sa;
    /* (src * sa + dst * (255 - sa) + 127) / 255 — round-half-up. */
    dst[0] = static_cast<std::uint8_t>(
        ((static_cast<std::uint32_t>(src[0]) * sa) +
         (static_cast<std::uint32_t>(dst[0]) * inv_sa) + 127) / 255);
    dst[1] = static_cast<std::uint8_t>(
        ((static_cast<std::uint32_t>(src[1]) * sa) +
         (static_cast<std::uint32_t>(dst[1]) * inv_sa) + 127) / 255);
    dst[2] = static_cast<std::uint8_t>(
        ((static_cast<std::uint32_t>(src[2]) * sa) +
         (static_cast<std::uint32_t>(dst[2]) * inv_sa) + 127) / 255);
    /* Alpha: 255 - (1-sa)(1-da) → sa + da*(1-sa) — keep dst's
     * existing alpha if it's higher, else compose. */
    const std::uint32_t da = dst[3];
    dst[3] = static_cast<std::uint8_t>(
        sa + (da * inv_sa + 127) / 255);
}

}  // namespace

me_status_t apply_face_sticker_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceStickerEffectParams&     params,
    std::span<const Bbox>                  landmark_bboxes,
    const std::uint8_t*                    sticker_rgba,
    int                                    sticker_width,
    int                                    sticker_height,
    std::size_t                            sticker_stride_bytes) {

    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }

    /* No bboxes / no sticker → no-op. The kernel is the data-driven
     * pixel mutation; if there's nothing to paste or nowhere to
     * paste it, the right answer is "leave the frame alone" + ME_OK
     * rather than ME_E_INVALID_ARG (the resolver upstream may
     * legitimately produce empty bbox lists for low-confidence
     * frames). */
    if (landmark_bboxes.empty()) return ME_OK;
    if (!sticker_rgba || sticker_width <= 0 || sticker_height <= 0) {
        return ME_OK;
    }
    if (sticker_stride_bytes < static_cast<std::size_t>(sticker_width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (params.scale_x <= 0.0 || params.scale_y <= 0.0) {
        /* Degenerate scale → sticker collapses to zero area; treat
         * as "fully hidden" (host should use opacity=0 instead but
         * we don't crash). */
        return ME_OK;
    }

    /* Scratch buffer for the scaled sticker — one bbox-sized RGBA8
     * patch reused across all bboxes (resized when a bigger bbox
     * appears). The affine_blit kernel writes transparent black
     * outside the sticker's mapped extent; we then alpha-over the
     * scratch onto rgba within the bbox region. */
    std::vector<std::uint8_t> scratch;

    for (const Bbox& bb : landmark_bboxes) {
        if (bb.empty()) continue;

        /* Clamp bbox to image bounds so the alpha-over loop doesn't
         * walk past `rgba`. */
        const int dst_x0 = std::max(0, static_cast<int>(bb.x0));
        const int dst_y0 = std::max(0, static_cast<int>(bb.y0));
        const int dst_x1 = std::min(width,  static_cast<int>(bb.x1));
        const int dst_y1 = std::min(height, static_cast<int>(bb.y1));
        if (dst_x1 <= dst_x0 || dst_y1 <= dst_y0) continue;

        const int patch_w = dst_x1 - dst_x0;
        const int patch_h = dst_y1 - dst_y0;
        const std::size_t patch_stride = static_cast<std::size_t>(patch_w) * 4;
        scratch.assign(patch_stride * patch_h, 0);

        /* Inverse affine: dst (patch) → src (sticker). The forward
         * transform we want: scale the sticker so it fills the bbox
         * at scale 1.0, then center it, then apply user offset.
         * Translation accounts for the bbox-clip offset (we
         * shifted dst origin to bb top-left, but the user expressed
         * offset in image-space). */
        const double bbox_w = static_cast<double>(bb.width());
        const double bbox_h = static_cast<double>(bb.height());
        const double final_scale_x =
            (bbox_w * params.scale_x) /
            static_cast<double>(sticker_width);
        const double final_scale_y =
            (bbox_h * params.scale_y) /
            static_cast<double>(sticker_height);

        /* Forward translate. The helper's `translate` parameter is
         * the post-anchor-add shift applied AFTER (anchor + R*S*(p -
         * anchor)). With anchor=(0.5, 0.5) (sticker center), the
         * forward maps sticker_center to (anchor_src + translate).
         * We want sticker_center to land at
         * (bbox_center_in_patch + user_offset), so:
         *   translate = (bbox_center_in_patch + user_offset)
         *               - anchor_src_in_pixels
         *             = (bbox_center_in_patch + user_offset)
         *               - (0.5 * sticker_w, 0.5 * sticker_h). */
        const double bbox_center_x_in_patch =
            static_cast<double>(bb.x0) + bbox_w * 0.5 -
            static_cast<double>(dst_x0);
        const double bbox_center_y_in_patch =
            static_cast<double>(bb.y0) + bbox_h * 0.5 -
            static_cast<double>(dst_y0);
        const double tx_in_patch =
            bbox_center_x_in_patch + params.offset_x -
            static_cast<double>(sticker_width)  * 0.5;
        const double ty_in_patch =
            bbox_center_y_in_patch + params.offset_y -
            static_cast<double>(sticker_height) * 0.5;

        /* Build inverse via the M2 transform helper, anchor=(0.5, 0.5)
         * so scale + rotate happen around the sticker's centre. */
        AffineMatrix inv = compose_inverse_affine(
            tx_in_patch, ty_in_patch,
            final_scale_x, final_scale_y,
            /*rotation_deg=*/0.0,
            /*anchor_x=*/0.5, /*anchor_y=*/0.5,
            sticker_width, sticker_height);

        affine_blit(
            scratch.data(), patch_w, patch_h, patch_stride,
            sticker_rgba, sticker_width, sticker_height, sticker_stride_bytes,
            inv);

        /* Source-over blend the scratch into dst at (dst_x0, dst_y0). */
        for (int row = 0; row < patch_h; ++row) {
            std::uint8_t* dst_row =
                rgba + static_cast<std::size_t>(dst_y0 + row) * stride_bytes +
                static_cast<std::size_t>(dst_x0) * 4;
            const std::uint8_t* src_row =
                scratch.data() + static_cast<std::size_t>(row) * patch_stride;
            for (int col = 0; col < patch_w; ++col) {
                blend_over_pixel(dst_row + col * 4, src_row + col * 4);
            }
        }
    }

    return ME_OK;
}

}  // namespace me::compose
