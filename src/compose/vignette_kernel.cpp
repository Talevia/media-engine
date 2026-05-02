/* vignette_kernel impl. See header for the contract. */
#include "compose/vignette_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace me::compose {

namespace {

float smoothstep(float a, float b, float x) {
    if (x <= a) return 0.0f;
    if (x >= b) return 1.0f;
    if (b == a) return (x >= a) ? 1.0f : 0.0f;
    const float t = (x - a) / (b - a);
    return t * t * (3.0f - 2.0f * t);
}

std::uint8_t scale_byte(std::uint8_t v, float factor) {
    if (factor <= 0.0f) return 0;
    if (factor >= 1.0f) return v;
    const float f = static_cast<float>(v) * factor + 0.5f;
    if (f <= 0.0f)  return 0;
    if (f >= 255.0f) return 255;
    return static_cast<std::uint8_t>(f);
}

}  // namespace

me_status_t apply_vignette_inplace(
    std::uint8_t*                   rgba,
    int                             width,
    int                             height,
    std::size_t                     stride_bytes,
    const me::VignetteEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (!std::isfinite(params.radius)    ||
        !std::isfinite(params.softness)  ||
        !std::isfinite(params.intensity) ||
        !std::isfinite(params.center_x)  ||
        !std::isfinite(params.center_y)) {
        return ME_E_INVALID_ARG;
    }
    if (params.radius < 0.0f || params.softness < 0.0f) return ME_E_INVALID_ARG;

    /* Identity early-out: no darkening at all. Negative
     * intensity also clamps to 0 → no-op. */
    const float intensity = std::max(params.intensity, 0.0f);
    if (intensity == 0.0f) return ME_OK;

    /* Radial distance is normalized so the smaller frame dim
     * spans [-1, 1]. Center expressed in normalized [0, 1]² is
     * mapped to the same [-1, 1] frame coordinate by 2 * (c -
     * 0.5). Scale factor along the larger dim is (larger /
     * smaller) so a 16:9 frame's corner reaches d ≈ 1.057. */
    const float min_dim = static_cast<float>(std::min(width, height));
    const float w_norm  = (width  >= height) ? (static_cast<float>(width)  / min_dim)
                                              : 1.0f;
    const float h_norm  = (height >= width)  ? (static_cast<float>(height) / min_dim)
                                              : 1.0f;
    const float cx = (2.0f * params.center_x - 1.0f) * w_norm;
    const float cy = (2.0f * params.center_y - 1.0f) * h_norm;

    const float r0 = params.radius;
    const float r1 = params.radius + params.softness;

    for (int y = 0; y < height; ++y) {
        const float py = (2.0f * (static_cast<float>(y) + 0.5f) /
                          static_cast<float>(height) - 1.0f) * h_norm;
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const float px = (2.0f * (static_cast<float>(x) + 0.5f) /
                              static_cast<float>(width) - 1.0f) * w_norm;
            const float dx = px - cx;
            const float dy = py - cy;
            const float d  = std::sqrt(dx * dx + dy * dy);
            const float t  = smoothstep(r0, r1, d);
            const float factor = 1.0f - intensity * t;
            std::uint8_t* pix = row + static_cast<std::size_t>(x) * 4;
            pix[0] = scale_byte(pix[0], factor);
            pix[1] = scale_byte(pix[1], factor);
            pix[2] = scale_byte(pix[2], factor);
            /* Alpha (pix[3]) unmodified. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
