/* `EffectKind::InverseTonemap` typed parameters — extracted from
 * `timeline_ir_params.hpp` (debt-split-...). Variant index 4.
 *
 * SDR → HDR inverse-tonemap effect parameters. The dual of
 * `TonemapEffectParams`. Per M10 exit criterion 6, the inverse
 * effect must be a REGISTERED kind even when the implementation
 * is intentionally deferred — so timeline JSON authoring tools
 * can include it ahead of the impl and the schema doesn't churn
 * later. The deferred impl is tracked under
 * `inverse-tonemap-hable-impl` (P2 backlog).
 *
 * Why this is non-deterministic. Inverse tonemap fundamentally
 * INVENTS information that the SDR input doesn't contain (specular
 * highlight detail collapsed onto byte 255, deep shadow texture
 * crushed into the bottom octave). Real implementations (HDR boost,
 * Dolby Content Mapping, etc.) use heuristics + neural networks
 * that are non-deterministic by construction; output depends on
 * GPU non-determinism, model weights at the time of call, etc.
 * `apply_inverse_tonemap_inplace` therefore returns
 * ME_E_UNSUPPORTED for the Hable algo today (see kernel header) —
 * VISION §3.1's deterministic-software-path contract requires us
 * NOT to ship a non-deterministic op into the default chain.
 *
 * `target_peak_nits` (≈ 1000 nits is a typical HDR10 mastering
 * peak) sets where SDR byte 255 maps to in the linear luminance
 * domain. `algo` reserves space for future curve choices —
 * starting with `Hable` as the inverse of the M10 `tonemap`
 * default, plus a `Linear` that's a no-op identity for testing
 * the registration path before any real expansion lands. */
#pragma once

#include <cstdint>

namespace me {

struct InverseTonemapEffectParams {
    enum class Algo : uint8_t {
        Linear = 0,    /* identity passthrough — registers the kind without
                        * doing any expansion (still ME_E_UNSUPPORTED today
                        * per the determinism rule). */
        Hable  = 1,    /* future: inverse of `TonemapEffectParams::Algo::Hable`. */
    };
    Algo   algo             = Algo::Linear;
    double target_peak_nits = 1000.0;   /* HDR10 mastering peak default. */
};

}  // namespace me
