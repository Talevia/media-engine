/* warp_kernel impl. See header. */
#include "compose/warp_kernel.hpp"

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

inline std::uint8_t bilinear_sample_channel(
    const std::uint8_t* src, int width, int height,
    std::size_t stride, double sx, double sy, int channel) {
    /* Clamp sample coords to image bounds. */
    if (sx < 0.0)               sx = 0.0;
    if (sx > width  - 1)        sx = width  - 1;
    if (sy < 0.0)               sy = 0.0;
    if (sy > height - 1)        sy = height - 1;

    const int   x0 = static_cast<int>(sx);
    const int   y0 = static_cast<int>(sy);
    const int   x1 = clamp_int(x0 + 1, 0, width  - 1);
    const int   y1 = clamp_int(y0 + 1, 0, height - 1);
    const double fx = sx - x0;
    const double fy = sy - y0;

    const std::uint8_t* p00 = src + static_cast<std::size_t>(y0) * stride
                                  + static_cast<std::size_t>(x0) * 4;
    const std::uint8_t* p10 = src + static_cast<std::size_t>(y0) * stride
                                  + static_cast<std::size_t>(x1) * 4;
    const std::uint8_t* p01 = src + static_cast<std::size_t>(y1) * stride
                                  + static_cast<std::size_t>(x0) * 4;
    const std::uint8_t* p11 = src + static_cast<std::size_t>(y1) * stride
                                  + static_cast<std::size_t>(x1) * 4;

    const double v00 = p00[channel];
    const double v10 = p10[channel];
    const double v01 = p01[channel];
    const double v11 = p11[channel];

    const double v0 = v00 * (1.0 - fx) + v10 * fx;
    const double v1 = v01 * (1.0 - fx) + v11 * fx;
    const double v  = v0  * (1.0 - fy) + v1  * fy;

    /* Round half-up to byte. */
    int r = static_cast<int>(std::floor(v + 0.5));
    return static_cast<std::uint8_t>(clamp_int(r, 0, 255));
}

}  // namespace

me_status_t apply_warp_inplace(
    std::uint8_t*               rgba,
    int                         width,
    int                         height,
    std::size_t                 stride_bytes,
    const me::WarpEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (params.control_points.size() > 32) return ME_E_INVALID_ARG;

    const auto& cps = params.control_points;

    /* Validate ranges + check identity. */
    bool all_identity = true;
    for (const auto& cp : cps) {
        if (!(cp.src_x >= 0.0f && cp.src_x <= 1.0f)) return ME_E_INVALID_ARG;
        if (!(cp.src_y >= 0.0f && cp.src_y <= 1.0f)) return ME_E_INVALID_ARG;
        if (!(cp.dst_x >= 0.0f && cp.dst_x <= 1.0f)) return ME_E_INVALID_ARG;
        if (!(cp.dst_y >= 0.0f && cp.dst_y <= 1.0f)) return ME_E_INVALID_ARG;
        if (cp.src_x != cp.dst_x || cp.src_y != cp.dst_y) all_identity = false;
    }
    if (cps.empty() || all_identity) return ME_OK;  /* identity */

    /* Convert normalized control points to absolute pixel coords
     * (one float→double conversion per control point, before the
     * per-pixel loop). */
    struct CP { double sx, sy, dx, dy, ddx, ddy; };
    std::vector<CP> abs_cps;
    abs_cps.reserve(cps.size());
    const double Wm1 = static_cast<double>(width  - 1);
    const double Hm1 = static_cast<double>(height - 1);
    for (const auto& cp : cps) {
        CP a;
        a.sx  = static_cast<double>(cp.src_x) * Wm1;
        a.sy  = static_cast<double>(cp.src_y) * Hm1;
        a.dx  = static_cast<double>(cp.dst_x) * Wm1;
        a.dy  = static_cast<double>(cp.dst_y) * Hm1;
        a.ddx = a.sx - a.dx;  /* displacement (src - dst) */
        a.ddy = a.sy - a.dy;
        abs_cps.push_back(a);
    }

    /* Snapshot input so in-place writes don't poison subsequent
     * reads. */
    std::vector<std::uint8_t> src(static_cast<std::size_t>(height) * stride_bytes);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row_in = rgba + static_cast<std::size_t>(y) * stride_bytes;
        std::uint8_t*       row_dst = src.data() +
            static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) * 4;
            row_dst[i + 0] = row_in[i + 0];
            row_dst[i + 1] = row_in[i + 1];
            row_dst[i + 2] = row_in[i + 2];
            row_dst[i + 3] = row_in[i + 3];
        }
    }

    constexpr double kEps = 1e-6;

    for (int y = 0; y < height; ++y) {
        std::uint8_t* out_row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        const double  py = static_cast<double>(y);
        for (int x = 0; x < width; ++x) {
            const double px = static_cast<double>(x);

            /* IDW interpolation. */
            double sum_w  = 0.0;
            double sum_dx = 0.0;
            double sum_dy = 0.0;
            bool   pinned = false;
            double pinned_ddx = 0.0;
            double pinned_ddy = 0.0;
            for (const auto& cp : abs_cps) {
                const double rx = px - cp.dx;
                const double ry = py - cp.dy;
                const double d2 = rx * rx + ry * ry;
                if (d2 < kEps) {
                    /* Pixel coincides with a control point's dst —
                     * source is exactly that cp.src. */
                    pinned     = true;
                    pinned_ddx = cp.ddx;
                    pinned_ddy = cp.ddy;
                    break;
                }
                const double w = 1.0 / d2;
                sum_w  += w;
                sum_dx += w * cp.ddx;
                sum_dy += w * cp.ddy;
            }

            double dx = 0.0, dy = 0.0;
            if (pinned) {
                dx = pinned_ddx;
                dy = pinned_ddy;
            } else if (sum_w > 0.0) {
                dx = sum_dx / sum_w;
                dy = sum_dy / sum_w;
            }

            const double sx = px + dx;
            const double sy = py + dy;

            std::uint8_t* op = out_row + static_cast<std::size_t>(x) * 4;
            op[0] = bilinear_sample_channel(src.data(), width, height,
                                             stride_bytes, sx, sy, 0);
            op[1] = bilinear_sample_channel(src.data(), width, height,
                                             stride_bytes, sx, sy, 1);
            op[2] = bilinear_sample_channel(src.data(), width, height,
                                             stride_bytes, sx, sy, 2);
            op[3] = bilinear_sample_channel(src.data(), width, height,
                                             stride_bytes, sx, sy, 3);
        }
    }
    return ME_OK;
}

}  // namespace me::compose
