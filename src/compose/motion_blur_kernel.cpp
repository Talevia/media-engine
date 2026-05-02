/* motion_blur_kernel impl. See header. */
#include "compose/motion_blur_kernel.hpp"

#include <cstdint>
#include <vector>

namespace me::compose {

namespace {

inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Integer divide rounded toward zero (matches default C++ /). */
inline int div_trunc(int numer, int denom) {
    return numer / denom;
}

}  // namespace

me_status_t apply_motion_blur_inplace(
    std::uint8_t*                     rgba,
    int                               width,
    int                               height,
    std::size_t                       stride_bytes,
    const me::MotionBlurEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (params.samples < 1 || params.samples > 64) return ME_E_INVALID_ARG;

    if (params.samples == 1 ||
        (params.dx_px == 0 && params.dy_px == 0)) {
        return ME_OK;  /* identity */
    }

    const int N    = params.samples;
    const int span = 2 * (N > 1 ? N - 1 : 1);  /* divisor for tap offsets */

    /* Snapshot input so in-place writes don't poison subsequent
     * taps. Cost: one image-sized copy. */
    std::vector<std::uint8_t> src(static_cast<std::size_t>(height) * stride_bytes);
    for (int y = 0; y < height; ++y) {
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

    for (int y = 0; y < height; ++y) {
        std::uint8_t* out_row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            int sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
            for (int i = 0; i < N; ++i) {
                const int k       = 2 * i - (N - 1);  /* signed, [-(N-1), +(N-1)] */
                const int off_x   = div_trunc(k * params.dx_px, span);
                const int off_y   = div_trunc(k * params.dy_px, span);
                const int sx      = clamp_int(x + off_x, 0, width  - 1);
                const int sy      = clamp_int(y + off_y, 0, height - 1);
                const std::uint8_t* sp = src.data()
                    + static_cast<std::size_t>(sy) * stride_bytes
                    + static_cast<std::size_t>(sx) * 4;
                sum_r += sp[0];
                sum_g += sp[1];
                sum_b += sp[2];
                sum_a += sp[3];
            }
            const int half = N / 2;  /* round-half-up via +half pre-divide */
            std::uint8_t* op = out_row + static_cast<std::size_t>(x) * 4;
            op[0] = static_cast<std::uint8_t>((sum_r + half) / N);
            op[1] = static_cast<std::uint8_t>((sum_g + half) / N);
            op[2] = static_cast<std::uint8_t>((sum_b + half) / N);
            op[3] = static_cast<std::uint8_t>((sum_a + half) / N);
        }
    }
    return ME_OK;
}

}  // namespace me::compose
