/*
 * tonemap_kernel impl. See header for contract.
 *
 * The three curves below are pure per-channel float math; no spatial
 * neighbourhood read, no parallelism, no FMA-dispatch. Same input →
 * same bytes (VISION §3.1 / §5.3 deterministic software path).
 */
#include "compose/tonemap_kernel.hpp"

#include <algorithm>
#include <cmath>

namespace me::compose {

namespace {

/* Hable filmic curve ("Uncharted 2"). Constants from
 * https://www.gdcvault.com/play/1012459/Uncharted_2__HDR_Lighting
 * (Hable 2010). Strong highlight roll-off + gentle shadow lift —
 * the colour-grader-favourite default. */
inline double hable_partial(double x) {
    constexpr double A = 0.15;
    constexpr double B = 0.50;
    constexpr double C = 0.10;
    constexpr double D = 0.20;
    constexpr double E = 0.02;
    constexpr double F = 0.30;
    return ((x * (A * x + C * B) + D * E)
          / (x * (A * x + B) + D * F)) - E / F;
}

inline double hable(double x) {
    /* White point at 11.2 (per Hable's published constants); the
     * curve is normalised by hable_partial(11.2) so the output white
     * lands at 1.0 for input >= 11.2. */
    constexpr double kWhite = 11.2;
    static const double white_scale = 1.0 / hable_partial(kWhite);
    return hable_partial(x) * white_scale;
}

/* Reinhard simple. `x / (1 + x)`. The simplest curve — gentle
 * compression of the entire range. Good when the input is mildly
 * over-bright but not aggressively HDR. */
inline double reinhard(double x) {
    return x / (1.0 + x);
}

/* ACES filmic approximation (Krzysztof Narkowicz, 2015 fit).
 * Closer to industry-standard ACES output but slightly clippy on
 * saturated reds. Numerator + denominator coefficients picked to
 * match ACES1.0 RRT+ODT within ~2% across the 0–10 nit range. */
inline double aces(double x) {
    constexpr double a = 2.51;
    constexpr double b = 0.03;
    constexpr double c = 2.43;
    constexpr double d = 0.59;
    constexpr double e = 0.14;
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e),
                      0.0, 1.0);
}

inline double dispatch(me::TonemapEffectParams::Algo algo, double x) {
    using A = me::TonemapEffectParams::Algo;
    switch (algo) {
    case A::Hable:    return hable(x);
    case A::Reinhard: return reinhard(x);
    case A::ACES:     return aces(x);
    }
    return x;
}

inline std::uint8_t round_clamp_u8(double v) {
    /* Symmetric round-half-to-even is overkill for 8-bit; nearest-
     * even via std::lround on the 0–255-scaled value is what every
     * consumer downstream of this kernel (PNG encode, blit) expects.
     * Clamp BEFORE the round so 254.7 doesn't become 255.0 and then
     * 255 (right answer) but 255.6 also becomes 255 (clamped before
     * round would be 256 → 255 anyway). The clamp inside the
     * formula already keeps `v` in [0, 1]; we scale + round here. */
    const double scaled = std::clamp(v, 0.0, 1.0) * 255.0;
    return static_cast<std::uint8_t>(std::lround(scaled));
}

}  // namespace

me_status_t apply_tonemap_inplace(std::uint8_t*                       rgba,
                                   int                                 width,
                                   int                                 height,
                                   std::size_t                         stride_bytes,
                                   const me::TonemapEffectParams&      params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (!(params.target_nits > 0.0)) return ME_E_INVALID_ARG;

    /* Input scale: byte → linear in [0, target_nits / 100]. The
     * 100 cd/m² reference is the SDR Rec.709 white point; scaling
     * the byte range past 1.0 lets `target_nits > 100` exercise the
     * highlight-roll-off region of each curve. */
    const double in_scale = (params.target_nits / 100.0) / 255.0;

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* px = row + x * 4;
            const double r_in = static_cast<double>(px[0]) * in_scale;
            const double g_in = static_cast<double>(px[1]) * in_scale;
            const double b_in = static_cast<double>(px[2]) * in_scale;
            px[0] = round_clamp_u8(dispatch(params.algo, r_in));
            px[1] = round_clamp_u8(dispatch(params.algo, g_in));
            px[2] = round_clamp_u8(dispatch(params.algo, b_in));
            /* px[3] (alpha) is photometrically meaningless; pass through. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
