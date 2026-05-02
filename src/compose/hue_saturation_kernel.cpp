/* hue_saturation_kernel impl. See header for the contract.
 *
 * Reference algorithm: standard sRGB ↔ HSL conversion (Wikipedia
 * "HSL and HSV" article). HSL is preferred over HSV because
 * the bullet's `lightness_scale` parameter implies the L
 * channel; HSV's V (max-component) doesn't have the
 * "perceptual lightness" interpretation users expect from a
 * "lightness" knob.
 *
 * Float operations are deterministic on IEEE-754 conformant
 * single-precision math without -ffast-math. The project's
 * current build flags (-Wall -Wextra -Wpedantic, no
 * -ffast-math, no FMA contract) preserve this guarantee. See
 * the header for the determinism caveat.
 */
#include "compose/hue_saturation_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace me::compose {

namespace {

/* Quantize float ∈ [0, 1] to uint8 with round-half-up. Out-of-
 * range floats clamp at the byte endpoints. Mirrors the
 * round-half-up integer divide other kernels use for byte-
 * domain quantization. */
std::uint8_t f01_to_byte(float f) {
    if (f <= 0.0f)  return 0;
    if (f >= 1.0f)  return 255;
    return static_cast<std::uint8_t>(f * 255.0f + 0.5f);
}

/* sRGB byte → HSL. RGB normalized to [0, 1]; H in [0, 360);
 * S, L in [0, 1]. Achromatic case (max == min): H = 0, S = 0,
 * L = max. */
void rgb_to_hsl(float r, float g, float b, float* h, float* s, float* l) {
    const float maxc = std::max({r, g, b});
    const float minc = std::min({r, g, b});
    const float sum  = maxc + minc;

    *l = sum * 0.5f;

    const float delta = maxc - minc;
    if (delta == 0.0f) {
        *h = 0.0f;
        *s = 0.0f;
        return;
    }
    /* S = delta / (1 - |2L - 1|) — but in normalized [0, 1]
     * this is delta / (max + min) for L < 0.5 and
     * delta / (2 - max - min) for L >= 0.5. */
    *s = (*l < 0.5f) ? (delta / sum)
                     : (delta / (2.0f - sum));

    /* Hue: which channel is the max? */
    if (maxc == r) {
        *h = 60.0f * (((g - b) / delta));
        if (*h < 0.0f) *h += 360.0f;
    } else if (maxc == g) {
        *h = 60.0f * (((b - r) / delta) + 2.0f);
    } else {
        *h = 60.0f * (((r - g) / delta) + 4.0f);
    }
    /* Wrap H to [0, 360). */
    if (*h >= 360.0f) *h -= 360.0f;
}

/* Helper: HSL → RGB intermediate (Wikipedia). t in [0, 1] is
 * the per-channel "test value"; q + p come from the
 * lightness/saturation. */
float hue_to_rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f)        return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

/* HSL → sRGB byte. */
void hsl_to_rgb(float h, float s, float l, float* r, float* g, float* b) {
    if (s == 0.0f) {
        *r = *g = *b = l;
        return;
    }
    const float q = (l < 0.5f) ? (l * (1.0f + s))
                                : (l + s - l * s);
    const float p = 2.0f * l - q;
    const float h_norm = h / 360.0f;
    *r = hue_to_rgb(p, q, h_norm + 1.0f / 3.0f);
    *g = hue_to_rgb(p, q, h_norm);
    *b = hue_to_rgb(p, q, h_norm - 1.0f / 3.0f);
}

}  // namespace

me_status_t apply_hue_saturation_inplace(
    std::uint8_t*                        rgba,
    int                                  width,
    int                                  height,
    std::size_t                          stride_bytes,
    const me::HueSaturationEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (!std::isfinite(params.hue_shift_deg)    ||
        !std::isfinite(params.saturation_scale) ||
        !std::isfinite(params.lightness_scale)) {
        return ME_E_INVALID_ARG;
    }

    /* Identity early-out. */
    if (params.hue_shift_deg == 0.0f &&
        params.saturation_scale == 1.0f &&
        params.lightness_scale  == 1.0f) {
        return ME_OK;
    }

    const float sat_scale = std::max(params.saturation_scale, 0.0f);
    const float lum_scale = std::max(params.lightness_scale,  0.0f);
    const float hue_shift = params.hue_shift_deg;

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;

            const float r0 = static_cast<float>(px[0]) / 255.0f;
            const float g0 = static_cast<float>(px[1]) / 255.0f;
            const float b0 = static_cast<float>(px[2]) / 255.0f;

            float h = 0.0f, s = 0.0f, l = 0.0f;
            rgb_to_hsl(r0, g0, b0, &h, &s, &l);

            /* Apply adjustments. Hue wraps; S and L clamp at
             * 1.0 on the high side. */
            h += hue_shift;
            h = std::fmod(h, 360.0f);
            if (h < 0.0f) h += 360.0f;
            s = std::min(s * sat_scale, 1.0f);
            l = std::min(l * lum_scale, 1.0f);

            float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
            hsl_to_rgb(h, s, l, &r1, &g1, &b1);

            px[0] = f01_to_byte(r1);
            px[1] = f01_to_byte(g1);
            px[2] = f01_to_byte(b1);
            /* Alpha (px[3]) unmodified. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
