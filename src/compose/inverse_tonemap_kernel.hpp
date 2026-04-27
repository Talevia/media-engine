/*
 * inverse_tonemap_kernel — SDR → HDR expansion (partial impl).
 *
 * Counterpart to `tonemap_kernel.{hpp,cpp}` (M10 exit criterion 6
 * second half — `inverse-tonemap-effect-stub`). Cycle 24 landed
 * `Linear`; `Hable` stays UNSUPPORTED (tracked as
 * `inverse-tonemap-hable-impl` — needs linear-light float
 * buffers).
 *
 * Algo semantics:
 *
 *   Linear  : `out = clamp(in/255 * (target_peak_nits/100), 0, 255)`
 *             per RGB channel, byte-deterministic. At target_peak_nits
 *             = 100 it's exact identity; higher values clip the
 *             upper part of the SDR signal as the byte domain
 *             saturates. Documented placeholder until a 16-bit
 *             working buffer arrives in M11+; the lost dynamic
 *             range is the price of byte-domain output. Real HDR
 *             expansion needs floats, so this is a "smoke test"-
 *             grade impl that exercises the registration path
 *             without claiming photographic correctness.
 *   Hable   : returns ME_E_UNSUPPORTED. Inverse Hable is invertible
 *             only on [0, white_scale] in linear-light space; byte
 *             rounding makes the round-trip non-deterministic
 *             enough to fail VISION §3.1.
 *
 * Why a partial impl. Real SDR → HDR expansion (HDR boost, Dolby
 * Content Mapping, DeepHDR neural networks) INVENTS information
 * that's structurally absent from the SDR input — those approaches
 * are non-deterministic by construction. Linear refuses to invent
 * anything: it's a deterministic linear scale that exists for
 * scenarios where a 16-bit pipeline downstream of the byte buffer
 * (e.g. `target_peak_nits=200` for partial expansion to a
 * mid-brightness HDR display) wants the linear scaling without
 * tone-curve invention.
 *
 * Determinism. Linear: pure scalar float arithmetic with std::lround,
 * no SIMD intrinsics, no parallelism. Same input → same output bytes.
 * VISION §3.1 satisfied.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* `Linear` algo writes deterministic output bytes; `Hable` algo
 * still returns ME_E_UNSUPPORTED until the linear-light buffer
 * tracked as `inverse-tonemap-hable-impl` lands. */
me_status_t apply_inverse_tonemap_inplace(
    std::uint8_t*                              rgba,
    int                                        width,
    int                                        height,
    std::size_t                                stride_bytes,
    const me::InverseTonemapEffectParams&      params);

}  // namespace me::compose
