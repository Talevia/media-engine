/* radial_blur_kernel impl. See header. */
#include "compose/radial_blur_kernel.hpp"

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

me_status_t apply_radial_blur_inplace(
    std::uint8_t*                     rgba,
    int                               width,
    int                               height,
    std::size_t                       stride_bytes,
    const me::RadialBlurEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (params.samples < 1 || params.samples > 64) return ME_E_INVALID_ARG;
    if (!(params.intensity >= 0.0f && params.intensity <= 1.0f)) {
        return ME_E_INVALID_ARG;
    }
    if (!(params.center_x >= 0.0f && params.center_x <= 1.0f)) {
        return ME_E_INVALID_ARG;
    }
    if (!(params.center_y >= 0.0f && params.center_y <= 1.0f)) {
        return ME_E_INVALID_ARG;
    }

    if (params.samples == 1 || params.intensity == 0.0f) {
        return ME_OK;  /* identity */
    }

    const int N = params.samples;
    /* Center in absolute pixel coords (clamped). */
    const int cx = clamp_int(round_half_up(params.center_x *
                                            static_cast<float>(width  - 1)),
                              0, width  - 1);
    const int cy = clamp_int(round_half_up(params.center_y *
                                            static_cast<float>(height - 1)),
                              0, height - 1);
    /* intensity in per-mille (0 .. 1000). */
    const int intensity_pm = clamp_int(round_half_up(params.intensity * 1000.0f),
                                        0, 1000);
    if (intensity_pm == 0) return ME_OK;

    /* Snapshot input so in-place writes don't poison subsequent
     * taps. */
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

    /* Precompute tap scale factors in per-mille. tap i factor =
     *   1000 + (2i - (N-1)) * intensity_pm / (N - 1)
     * (truncating divide). i ∈ [0, N). */
    const int Nm1 = N - 1;
    std::vector<int> scale_pm(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        const int k = 2 * i - Nm1;
        scale_pm[static_cast<std::size_t>(i)] =
            1000 + (k * intensity_pm) / Nm1;
    }

    for (int y = 0; y < height; ++y) {
        std::uint8_t* out_row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;

            int sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
            for (int i = 0; i < N; ++i) {
                const int s = scale_pm[static_cast<std::size_t>(i)];
                /* tap = center + d * s/1000, signed integer divide. */
                const int tx = clamp_int(cx + (dx * s) / 1000, 0, width  - 1);
                const int ty = clamp_int(cy + (dy * s) / 1000, 0, height - 1);
                const std::uint8_t* sp = src.data()
                    + static_cast<std::size_t>(ty) * stride_bytes
                    + static_cast<std::size_t>(tx) * 4;
                sum_r += sp[0];
                sum_g += sp[1];
                sum_b += sp[2];
                sum_a += sp[3];
            }
            const int half = N / 2;
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
