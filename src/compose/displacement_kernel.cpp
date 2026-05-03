/* displacement_kernel impl. See header. */
#include "compose/displacement_kernel.hpp"

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

inline std::uint8_t bilinear_channel(
    const std::uint8_t* src, int width, int height,
    std::size_t stride, double sx, double sy, int channel) {
    if (sx < 0.0)               sx = 0.0;
    if (sx > width  - 1)        sx = width  - 1;
    if (sy < 0.0)               sy = 0.0;
    if (sy > height - 1)        sy = height - 1;

    const int    x0 = static_cast<int>(sx);
    const int    y0 = static_cast<int>(sy);
    const int    x1 = clamp_int(x0 + 1, 0, width  - 1);
    const int    y1 = clamp_int(y0 + 1, 0, height - 1);
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

    int r = static_cast<int>(std::floor(v + 0.5));
    return static_cast<std::uint8_t>(clamp_int(r, 0, 255));
}

}  // namespace

me_status_t apply_displacement_inplace(
    std::uint8_t*       rgba,
    int                 width,
    int                 height,
    std::size_t         stride_bytes,
    const std::uint8_t* tex_rgba,
    int                 tex_width,
    int                 tex_height,
    float               strength_x,
    float               strength_y) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;

    if (strength_x == 0.0f && strength_y == 0.0f) return ME_OK;  /* identity */

    if (!tex_rgba || tex_width <= 0 || tex_height <= 0) return ME_E_INVALID_ARG;

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

    const std::size_t tex_stride = static_cast<std::size_t>(tex_width) * 4;
    const double Wm1   = static_cast<double>(width  - 1);
    const double Hm1   = static_cast<double>(height - 1);
    const double TWm1  = static_cast<double>(tex_width  - 1);
    const double THm1  = static_cast<double>(tex_height - 1);
    const double sx_d  = static_cast<double>(strength_x);
    const double sy_d  = static_cast<double>(strength_y);

    for (int y = 0; y < height; ++y) {
        std::uint8_t* out_row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        const double  py = static_cast<double>(y);
        /* Map output row to texture row coord. */
        const double ty_norm = (Hm1 > 0.0) ? (py / Hm1) : 0.0;
        const double ty      = ty_norm * THm1;
        for (int x = 0; x < width; ++x) {
            const double px = static_cast<double>(x);
            const double tx_norm = (Wm1 > 0.0) ? (px / Wm1) : 0.0;
            const double tx      = tx_norm * TWm1;

            const std::uint8_t r = bilinear_channel(tex_rgba, tex_width, tex_height,
                                                     tex_stride, tx, ty, 0);
            const std::uint8_t g = bilinear_channel(tex_rgba, tex_width, tex_height,
                                                     tex_stride, tx, ty, 1);

            const double signed_r = static_cast<double>(r) * 2.0 - 255.0;
            const double signed_g = static_cast<double>(g) * 2.0 - 255.0;
            const double off_x    = (signed_r * sx_d) / 255.0;
            const double off_y    = (signed_g * sy_d) / 255.0;

            const double sample_x = px + off_x;
            const double sample_y = py + off_y;

            std::uint8_t* op = out_row + static_cast<std::size_t>(x) * 4;
            op[0] = bilinear_channel(src.data(), width, height, stride_bytes,
                                      sample_x, sample_y, 0);
            op[1] = bilinear_channel(src.data(), width, height, stride_bytes,
                                      sample_x, sample_y, 1);
            op[2] = bilinear_channel(src.data(), width, height, stride_bytes,
                                      sample_x, sample_y, 2);
            op[3] = bilinear_channel(src.data(), width, height, stride_bytes,
                                      sample_x, sample_y, 3);
        }
    }
    return ME_OK;
}

}  // namespace me::compose
