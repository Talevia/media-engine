/* tilt_shift_kernel impl. See header. */
#include "compose/tilt_shift_kernel.hpp"

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

inline int round_half_up(float f) {
    return static_cast<int>(std::floor(f + 0.5f));
}

}  // namespace

me_status_t apply_tilt_shift_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::TiltShiftEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (!(params.focal_y_min >= 0.0f && params.focal_y_min <= 1.0f)) {
        return ME_E_INVALID_ARG;
    }
    if (!(params.focal_y_max >= 0.0f && params.focal_y_max <= 1.0f)) {
        return ME_E_INVALID_ARG;
    }
    if (params.focal_y_min > params.focal_y_max) return ME_E_INVALID_ARG;
    if (!(params.edge_softness > 0.0f && params.edge_softness <= 1.0f)) {
        return ME_E_INVALID_ARG;
    }
    if (params.max_blur_radius < 0 || params.max_blur_radius > 32) {
        return ME_E_INVALID_ARG;
    }

    if (params.max_blur_radius == 0) return ME_OK;  /* identity */

    /* Convert float fields to per-mille integers once. */
    const int H            = height;
    const int focal_min_pm = clamp_int(round_half_up(params.focal_y_min  * 1000.0f),
                                        0, 1000);
    const int focal_max_pm = clamp_int(round_half_up(params.focal_y_max  * 1000.0f),
                                        focal_min_pm, 1000);
    const int softness_pm  = clamp_int(round_half_up(params.edge_softness * 1000.0f),
                                        1, 1000);
    const int Rmax         = params.max_blur_radius;

    /* Precompute per-row radius. */
    std::vector<int> radius_per_row(static_cast<std::size_t>(H));
    for (int y = 0; y < H; ++y) {
        /* Normalized y in per-mille: (y * 1000) / (H - 1) when H > 1,
         * else 0. Row 0 maps to 0, row H-1 maps to 1000. */
        const int y_pm = (H > 1)
            ? (y * 1000) / (H - 1)
            : 0;
        int d_pm = 0;
        if (y_pm < focal_min_pm) d_pm = focal_min_pm - y_pm;
        else if (y_pm > focal_max_pm) d_pm = y_pm - focal_max_pm;
        /* t = clamp(d / softness, 0, 1) — in per-mille:
         *   t_pm = clamp((d_pm * 1000) / softness_pm, 0, 1000) */
        const int t_pm = clamp_int((d_pm * 1000) / softness_pm, 0, 1000);
        /* r = round((t_pm / 1000) * Rmax) = (t_pm * Rmax + 500) / 1000 */
        const int r = (t_pm * Rmax + 500) / 1000;
        radius_per_row[static_cast<std::size_t>(y)] = clamp_int(r, 0, Rmax);
    }

    /* Snapshot input so in-place writes don't poison subsequent
     * box reads. */
    std::vector<std::uint8_t> src(static_cast<std::size_t>(H) * stride_bytes);
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        std::uint8_t*       dst = src.data() + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) * 4;
            dst[i + 0] = row[i + 0];
            dst[i + 1] = row[i + 1];
            dst[i + 2] = row[i + 2];
            dst[i + 3] = row[i + 3];
        }
    }

    for (int y = 0; y < H; ++y) {
        const int r = radius_per_row[static_cast<std::size_t>(y)];
        std::uint8_t* out_row = rgba + static_cast<std::size_t>(y) * stride_bytes;

        if (r == 0) {
            /* No blur: in-focus row stays exactly equal to source. */
            const std::uint8_t* src_row = src.data() +
                static_cast<std::size_t>(y) * stride_bytes;
            for (int x = 0; x < width; ++x) {
                const std::size_t i = static_cast<std::size_t>(x) * 4;
                out_row[i + 0] = src_row[i + 0];
                out_row[i + 1] = src_row[i + 1];
                out_row[i + 2] = src_row[i + 2];
                out_row[i + 3] = src_row[i + 3];
            }
            continue;
        }

        const int box_w = 2 * r + 1;
        const int box_n = box_w * box_w;
        const int half  = box_n / 2;

        for (int x = 0; x < width; ++x) {
            int sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
            for (int dy = -r; dy <= r; ++dy) {
                const int sy = clamp_int(y + dy, 0, H - 1);
                const std::uint8_t* sp_row = src.data() +
                    static_cast<std::size_t>(sy) * stride_bytes;
                for (int dx = -r; dx <= r; ++dx) {
                    const int sx = clamp_int(x + dx, 0, width - 1);
                    const std::uint8_t* sp = sp_row +
                        static_cast<std::size_t>(sx) * 4;
                    sum_r += sp[0];
                    sum_g += sp[1];
                    sum_b += sp[2];
                    sum_a += sp[3];
                }
            }
            std::uint8_t* op = out_row + static_cast<std::size_t>(x) * 4;
            op[0] = static_cast<std::uint8_t>((sum_r + half) / box_n);
            op[1] = static_cast<std::uint8_t>((sum_g + half) / box_n);
            op[2] = static_cast<std::uint8_t>((sum_b + half) / box_n);
            op[3] = static_cast<std::uint8_t>((sum_a + half) / box_n);
        }
    }
    return ME_OK;
}

}  // namespace me::compose
