/*
 * warp_kernel — control-point inverse-mapping warp.
 *
 * M12 §158 (1/2 geometric effects). For each output pixel,
 * compute its source position via inverse-distance-weighted
 * (IDW, exponent = 2) interpolation across the control-point
 * (src, dst) pairs, then sample the input bilinearly at that
 * source position. Edges clamp.
 *
 * IDW reconstruction:
 *   For control point i, displacement vector
 *     d_i = src_i - dst_i (in pixel coords)
 *   is what "moves" the destination back to source. For an
 *   output pixel at p:
 *     w_i = 1 / (|p - dst_i|² + eps)
 *     delta(p) = sum(w_i * d_i) / sum(w_i)
 *     source_pos(p) = p + delta(p)
 *   Special case: if p coincides with any dst_i (within eps),
 *   the sum collapses to that point and source_pos = src_i
 *   exactly (interpolation property).
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / w/h <= 0          → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4            → ME_E_INVALID_ARG
 *   - any control_point coord outside [0, 1] → ME_E_INVALID_ARG
 *   - control_points.size() > 32          → ME_E_INVALID_ARG
 *
 * Empty control_points OR all src == dst → identity early-out.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_warp_inplace(
    std::uint8_t*               rgba,
    int                         width,
    int                         height,
    std::size_t                 stride_bytes,
    const me::WarpEffectParams& params);

}  // namespace me::compose
