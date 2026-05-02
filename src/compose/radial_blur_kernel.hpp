/*
 * radial_blur_kernel — zoom-style radial blur.
 *
 * M12 §157 (2/3 blur variants). Pure-pixel kernel. For each
 * output pixel (x, y), sample along the line from the radial
 * center through the pixel position, scaling the pixel-to-
 * center vector by a per-tap factor that ranges over
 *   [1 - intensity, 1 + intensity]
 * symmetrically. Average the samples (round-half-up).
 *
 * Float fields (center_x/y, intensity) are converted to
 * integers exactly once on entry; the per-pixel hot loop is
 * pure integer math. Out-of-bounds taps clamp at edges.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / w/h <= 0      → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4        → ME_E_INVALID_ARG
 *   - samples < 1 OR samples > 64     → ME_E_INVALID_ARG
 *   - intensity < 0 OR intensity > 1  → ME_E_INVALID_ARG
 *   - center_x/y not in [0, 1]        → ME_E_INVALID_ARG
 *
 * samples == 1 OR intensity == 0 → identity early-out.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_radial_blur_inplace(
    std::uint8_t*                     rgba,
    int                               width,
    int                               height,
    std::size_t                       stride_bytes,
    const me::RadialBlurEffectParams& params);

}  // namespace me::compose
