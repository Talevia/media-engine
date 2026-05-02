/*
 * tilt_shift_kernel — depth-of-field band blur.
 *
 * M12 §157 (3/3 blur variants). Pure-pixel kernel. Per-row
 * blur radius:
 *   r(y) = round( clamp((distance from focal band) /
 *                       edge_softness, 0, 1) * max_blur_radius )
 * (within the focal band: r = 0).
 *
 * For each pixel (x, y), the output is the (2r+1)x(2r+1) box
 * average centered at (x, y) reading from a snapshot of the
 * input. Edges clamp.
 *
 * Determinism: float fields converted to per-mille integers
 * once on entry; per-row r computed in integer; per-pixel hot
 * loop is integer-only.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / w/h <= 0          → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4            → ME_E_INVALID_ARG
 *   - focal_y_* outside [0, 1]            → ME_E_INVALID_ARG
 *   - focal_y_min > focal_y_max           → ME_E_INVALID_ARG
 *   - edge_softness <= 0 OR > 1           → ME_E_INVALID_ARG
 *   - max_blur_radius < 0 OR > 32         → ME_E_INVALID_ARG
 *
 * max_blur_radius == 0 → identity early-out.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_tilt_shift_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::TiltShiftEffectParams& params);

}  // namespace me::compose
