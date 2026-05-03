/* decode_selfie_segmentation_mask impl. See header. */
#include "compose/selfie_segmentation_decode.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace me::compose {

namespace {

inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline std::uint8_t bilinear_alpha_sample(
    const std::vector<std::uint8_t>& src, int src_w, int src_h,
    double sx, double sy) {
    if (sx < 0.0)         sx = 0.0;
    if (sx > src_w - 1)   sx = src_w - 1;
    if (sy < 0.0)         sy = 0.0;
    if (sy > src_h - 1)   sy = src_h - 1;

    const int    x0 = static_cast<int>(sx);
    const int    y0 = static_cast<int>(sy);
    const int    x1 = clamp_int(x0 + 1, 0, src_w - 1);
    const int    y1 = clamp_int(y0 + 1, 0, src_h - 1);
    const double fx = sx - x0;
    const double fy = sy - y0;

    const double v00 = src[static_cast<std::size_t>(y0) * src_w + x0];
    const double v10 = src[static_cast<std::size_t>(y0) * src_w + x1];
    const double v01 = src[static_cast<std::size_t>(y1) * src_w + x0];
    const double v11 = src[static_cast<std::size_t>(y1) * src_w + x1];

    const double v0 = v00 * (1.0 - fx) + v10 * fx;
    const double v1 = v01 * (1.0 - fx) + v11 * fx;
    const double v  = v0  * (1.0 - fy) + v1  * fy;

    int r = static_cast<int>(std::floor(v + 0.5));
    return static_cast<std::uint8_t>(clamp_int(r, 0, 255));
}

bool extract_hw_from_shape(const std::vector<std::int64_t>& shape,
                            int* out_h, int* out_w) {
    /* {1, 1, H, W} (NCHW) — channels = 1. */
    if (shape.size() == 4 && shape[0] == 1 && shape[1] == 1 &&
        shape[2] > 0 && shape[3] > 0) {
        *out_h = static_cast<int>(shape[2]);
        *out_w = static_cast<int>(shape[3]);
        return true;
    }
    /* {1, H, W, 1} (NHWC) — channels = 1 trailing. */
    if (shape.size() == 4 && shape[0] == 1 && shape[3] == 1 &&
        shape[1] > 0 && shape[2] > 0) {
        *out_h = static_cast<int>(shape[1]);
        *out_w = static_cast<int>(shape[2]);
        return true;
    }
    /* {1, H, W} — single-channel without explicit channel axis. */
    if (shape.size() == 3 && shape[0] == 1 &&
        shape[1] > 0 && shape[2] > 0) {
        *out_h = static_cast<int>(shape[1]);
        *out_w = static_cast<int>(shape[2]);
        return true;
    }
    return false;
}

}  // namespace

me_status_t decode_selfie_segmentation_mask(
    const me::inference::Tensor& logits,
    int                          target_width,
    int                          target_height,
    int*                         out_mask_width,
    int*                         out_mask_height,
    std::vector<std::uint8_t>*   out_alpha,
    std::string*                 err) {
    if (!out_mask_width || !out_mask_height || !out_alpha) {
        return ME_E_INVALID_ARG;
    }
    *out_mask_width  = 0;
    *out_mask_height = 0;
    out_alpha->clear();

    if (target_width <= 0 || target_height <= 0) return ME_E_INVALID_ARG;

    if (logits.dtype != me::inference::Dtype::Float32) {
        if (err) *err = "decode_selfie_segmentation_mask: expected Float32 logits";
        return ME_E_INVALID_ARG;
    }

    int H = 0, W = 0;
    if (!extract_hw_from_shape(logits.shape, &H, &W)) {
        if (err) *err = "decode_selfie_segmentation_mask: unsupported logit shape "
                        "(expected NCHW {1,1,H,W} / NHWC {1,H,W,1} / {1,H,W})";
        return ME_E_INVALID_ARG;
    }

    const std::size_t expected_bytes =
        static_cast<std::size_t>(H) * W * 4;
    if (logits.bytes.size() != expected_bytes) {
        if (err) *err = "decode_selfie_segmentation_mask: logits.bytes.size() "
                        "doesn't match shape product * 4 bytes";
        return ME_E_INVALID_ARG;
    }

    /* Step 1+2: sigmoid + quantize the logits into a (H, W) uint8
     * buffer. */
    std::vector<std::uint8_t> mask_lo(static_cast<std::size_t>(H) * W);
    const auto* fp = reinterpret_cast<const float*>(logits.bytes.data());
    for (std::size_t i = 0; i < static_cast<std::size_t>(H) * W; ++i) {
        const double  l   = static_cast<double>(fp[i]);
        const double  p   = 1.0 / (1.0 + std::exp(-l));
        const int     q   = static_cast<int>(std::floor(p * 255.0 + 0.5));
        mask_lo[i] = static_cast<std::uint8_t>(clamp_int(q, 0, 255));
    }

    /* Step 3: bilinear upscale from (H, W) to (target_w, target_h).
     * If the dimensions match, copy the buffer directly. */
    if (H == target_height && W == target_width) {
        *out_mask_width  = target_width;
        *out_mask_height = target_height;
        *out_alpha       = std::move(mask_lo);
        return ME_OK;
    }

    out_alpha->assign(
        static_cast<std::size_t>(target_width) * target_height, 0);

    const double scale_x = (target_width  > 1)
        ? static_cast<double>(W - 1) / (target_width  - 1)
        : 0.0;
    const double scale_y = (target_height > 1)
        ? static_cast<double>(H - 1) / (target_height - 1)
        : 0.0;

    for (int y = 0; y < target_height; ++y) {
        const double sy = scale_y * y;
        for (int x = 0; x < target_width; ++x) {
            const double sx = scale_x * x;
            (*out_alpha)[static_cast<std::size_t>(y) * target_width + x] =
                bilinear_alpha_sample(mask_lo, W, H, sx, sy);
        }
    }
    *out_mask_width  = target_width;
    *out_mask_height = target_height;
    return ME_OK;
}

}  // namespace me::compose
