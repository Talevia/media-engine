/*
 * posterize_kernel — per-channel quantization to N levels.
 *
 * M12 §156 (4/5 stylized effects). Pure-pixel kernel.
 * `levels=256` early-outs (identity); alpha unmodified.
 *
 * Per-pixel formula (round-half-up integer math, no float
 * in the byte-output path):
 *   bucket  = (in * (levels - 1) + 127) / 255
 *   out     = (bucket * 255 + (levels - 1) / 2) / (levels - 1)
 *
 * The double divide ensures bucketing is symmetric: byte 128
 * always maps to bucket round(128 * (L-1) / 255) for any L,
 * and the resulting bucket-representative byte is the
 * uniform-spaced quantum rounded back to [0, 255].
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / w/h <= 0          → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4            → ME_E_INVALID_ARG
 *   - levels < 2 OR levels > 256          → ME_E_INVALID_ARG
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_posterize_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::PosterizeEffectParams& params);

}  // namespace me::compose
