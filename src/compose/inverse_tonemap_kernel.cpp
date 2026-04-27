/*
 * inverse_tonemap_kernel impl — partial.
 *
 * Algo dispatch:
 *
 *   Linear (cycle 24 +): `out = clamp(in/255 * (target_peak_nits/100),
 *                          0, 255)` per RGB channel; alpha passthrough.
 *                        Deterministic linear-light scale; loses HDR
 *                        range above 100 cd/m² × (255 / target_peak_nits)
 *                        as bytes saturate to 255 — the documented
 *                        placeholder semantics until a 16-bit working
 *                        buffer arrives in M11+.
 *
 *   Hable  (deferred):    `inverse-tonemap-hable-impl` follow-up
 *                        bullet — needs linear-light float buffers
 *                        because the inverse Hable curve is invertible
 *                        only on the [0, white_scale] domain and in
 *                        byte space the rounding loss makes the
 *                        round-trip non-deterministic.
 *
 * Argument-shape rejects (null / non-positive dims / undersized
 * stride / non-positive target_peak_nits) take precedence over algo
 * dispatch — same prologue as `apply_tonemap_inplace`.
 *
 * Why no err out param: matches `apply_tonemap_inplace`'s minimal
 * signature. The Hable follow-up tracker lives in the BACKLOG.
 */
#include "compose/inverse_tonemap_kernel.hpp"

#include <algorithm>
#include <cmath>

namespace me::compose {

namespace {

/* Linear scale: SDR byte normalised to [0, 1] × headroom factor →
 * clamp back to byte. headroom = target_peak_nits / 100. At
 * target_peak_nits=100 this is exact identity (multiply by 1). At
 * target_peak_nits=1000 (HDR10 mastering peak) it's a ×10 boost
 * that clips above byte 26 — the documented "lose range" tradeoff
 * for byte-domain output. Round-half-to-even via std::lround. */
inline std::uint8_t linear_scale_byte(std::uint8_t in, double headroom) {
    const double v = (static_cast<double>(in) / 255.0) * headroom * 255.0;
    const long   q = std::lround(v);
    return static_cast<std::uint8_t>(std::clamp<long>(q, 0, 255));
}

}  // namespace

me_status_t apply_inverse_tonemap_inplace(
    std::uint8_t*                              rgba,
    int                                        width,
    int                                        height,
    std::size_t                                stride_bytes,
    const me::InverseTonemapEffectParams&      params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (!(params.target_peak_nits > 0.0)) return ME_E_INVALID_ARG;

    using Algo = me::InverseTonemapEffectParams::Algo;

    if (params.algo == Algo::Hable) {
        /* LEGIT: Hable inverse needs linear-light float buffers — see
         * `inverse-tonemap-hable-impl` BACKLOG bullet. Byte-domain
         * inverse rounds enough that round-trip drift fails VISION
         * §3.1 byte-identity; deferred until M11+ float pipeline. */
        return ME_E_UNSUPPORTED;
    }

    /* Linear: byte-deterministic linear scale by target_peak_nits/100. */
    const double headroom = params.target_peak_nits / 100.0;
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            px[0] = linear_scale_byte(px[0], headroom);
            px[1] = linear_scale_byte(px[1], headroom);
            px[2] = linear_scale_byte(px[2], headroom);
            /* alpha: passthrough — opacity isn't a tonal value. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
