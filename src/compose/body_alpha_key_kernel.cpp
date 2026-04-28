/*
 * body_alpha_key_kernel — full impl. See header for the
 * pre-resolved-mask contract.
 */
#include "compose/body_alpha_key_kernel.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace me::compose {

namespace {

/* Two-pass separable box blur on a single-channel 8-bit buffer.
 * Reads `src` (size w × h, stride src_stride), writes `dst` (same
 * dims, stride dst_stride). Edge clamping at the buffer borders.
 * Deterministic integer math with round-half-up. */
void box_blur_alpha(
    const std::uint8_t* src, std::size_t src_stride,
    std::uint8_t*       dst, std::size_t dst_stride,
    int w, int h, int radius) {

    if (radius <= 0) {
        for (int y = 0; y < h; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * dst_stride,
                         src + static_cast<std::size_t>(y) * src_stride,
                         static_cast<std::size_t>(w));
        }
        return;
    }

    /* Horizontal pass: src → tmp. */
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* sr = src + static_cast<std::size_t>(y) * src_stride;
        std::uint8_t* tr = tmp.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const int lo = std::max(0,     x - radius);
            const int hi = std::min(w - 1, x + radius);
            const int n  = hi - lo + 1;
            std::uint32_t sum = 0;
            for (int xi = lo; xi <= hi; ++xi) sum += sr[xi];
            const std::uint32_t half = static_cast<std::uint32_t>(n) / 2;
            tr[x] = static_cast<std::uint8_t>((sum + half) / n);
        }
    }
    /* Vertical pass: tmp → dst. */
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            const int lo = std::max(0,     y - radius);
            const int hi = std::min(h - 1, y + radius);
            const int n  = hi - lo + 1;
            std::uint32_t sum = 0;
            for (int yi = lo; yi <= hi; ++yi) {
                sum += tmp[static_cast<std::size_t>(yi) * w + x];
            }
            const std::uint32_t half = static_cast<std::uint32_t>(n) / 2;
            dst[static_cast<std::size_t>(y) * dst_stride + x] =
                static_cast<std::uint8_t>((sum + half) / n);
        }
    }
}

}  // namespace

me_status_t apply_body_alpha_key_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::BodyAlphaKeyEffectParams&    params,
    const std::uint8_t*                    mask,
    int                                    mask_width,
    int                                    mask_height,
    std::size_t                            mask_stride) {

    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (params.feather_radius_px < 0) return ME_E_INVALID_ARG;

    /* Null mask → no-op. The resolver may legitimately fail to
     * produce a mask for a frame (e.g. low-confidence
     * segmentation); the right answer is "leave the frame alone"
     * + ME_OK rather than INVALID_ARG. */
    if (!mask) return ME_OK;

    if (mask_width != width || mask_height != height) {
        return ME_E_INVALID_ARG;
    }
    if (mask_stride < static_cast<std::size_t>(mask_width)) {
        return ME_E_INVALID_ARG;
    }

    /* Build the effective mask: invert + feather. We materialize a
     * full-frame uint8 buffer because subsequent per-pixel reads
     * are easier than walking the original `mask` indirectly. */
    std::vector<std::uint8_t> effective(
        static_cast<std::size_t>(width) * height);
    const std::size_t eff_stride = static_cast<std::size_t>(width);

    /* Invert pass: copy mask into effective with optional 255-x. */
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* m_row = mask + static_cast<std::size_t>(y) * mask_stride;
        std::uint8_t* e_row = effective.data() + static_cast<std::size_t>(y) * eff_stride;
        if (params.invert) {
            for (int x = 0; x < width; ++x) {
                e_row[x] = static_cast<std::uint8_t>(255 - m_row[x]);
            }
        } else {
            std::memcpy(e_row, m_row, static_cast<std::size_t>(width));
        }
    }

    /* Feather: box-blur using a second scratch buffer, swap in. */
    if (params.feather_radius_px > 0) {
        std::vector<std::uint8_t> blurred(
            static_cast<std::size_t>(width) * height);
        box_blur_alpha(
            effective.data(), eff_stride,
            blurred.data(),   eff_stride,
            width, height, params.feather_radius_px);
        effective.swap(blurred);
    }

    /* Apply: output.alpha = (input.alpha * effective + 127) / 255. */
    for (int y = 0; y < height; ++y) {
        std::uint8_t* dst_row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        const std::uint8_t* eff_row = effective.data() +
            static_cast<std::size_t>(y) * eff_stride;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* px = dst_row + static_cast<std::size_t>(x) * 4;
            const std::uint32_t a = px[3];
            const std::uint32_t m = eff_row[x];
            px[3] = static_cast<std::uint8_t>((a * m + 127) / 255);
        }
    }

    return ME_OK;
}

}  // namespace me::compose
