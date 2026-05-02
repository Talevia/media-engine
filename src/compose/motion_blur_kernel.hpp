/*
 * motion_blur_kernel — linear-direction tap-average blur.
 *
 * M12 §157 (1/3 blur variants). Pure-pixel kernel. For each
 * output pixel (x, y), accumulate `samples` reads along an
 * integer-pixel line centered on (x, y) with total span
 * (dx_px, dy_px); take the integer mean.
 *
 * Tap positions:
 *   tap[i].x = x + ( (2i - (samples - 1)) * dx_px ) /
 *              ( 2 * max(samples - 1, 1) )
 *   tap[i].y = y + ( (2i - (samples - 1)) * dy_px ) /
 *              ( 2 * max(samples - 1, 1) )
 *   for i ∈ [0, samples).
 * (Bankers'-rounded toward zero.) Out-of-bounds taps clamp.
 *
 * Determinism: pure integer math; no PRNG, no FP. Reads from
 * a precomputed snapshot of the input so the in-place mode
 * doesn't bias toward already-written taps. Alpha is averaged
 * the same way as RGB.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / w/h <= 0     → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4       → ME_E_INVALID_ARG
 *   - samples < 1 OR samples > 64    → ME_E_INVALID_ARG
 *
 * samples == 1 OR (dx_px == 0 AND dy_px == 0) → identity
 * (early-out).
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_motion_blur_inplace(
    std::uint8_t*                     rgba,
    int                               width,
    int                               height,
    std::size_t                       stride_bytes,
    const me::MotionBlurEffectParams& params);

}  // namespace me::compose
