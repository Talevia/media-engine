/*
 * vignette_kernel — radial darkening centered on a configurable
 * point with smoothstep falloff.
 *
 * M12 §155 (3/4 color effects). Pure-pixel kernel: no asset
 * deps, no time dep. Identity intensity (0.0) early-out skips
 * the per-pixel pass; alpha is unmodified.
 *
 * Per-pixel formula (in normalized coords where the smaller
 * frame dim spans [-1, 1]):
 *   d = sqrt((x - cx)² + (y - cy)²)
 *   t = smoothstep(radius, radius + softness, d)
 *   factor = 1 - intensity * t
 *   out = round(in * factor)  (clamp 0..255 per channel)
 *
 * smoothstep(a, b, x) = clamp((x - a) / (b - a), 0, 1) ^ 2 * (3 - 2 * ratio)
 *
 * Determinism. Float arithmetic for the radial distance +
 * smoothstep; quantize via round-half-up at output. Same
 * caveat as hue_saturation_kernel — IEEE-754 single-precision
 * deterministic on the project's build flags.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0 → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4              → ME_E_INVALID_ARG.
 *   - non-finite params                     → ME_E_INVALID_ARG.
 *   - radius < 0 OR softness < 0            → ME_E_INVALID_ARG.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_vignette_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::VignetteEffectParams&  params);

}  // namespace me::compose
