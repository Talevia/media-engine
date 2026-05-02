/*
 * chromatic_aberration_kernel — per-channel pixel offset.
 *
 * M12 §156 (3/5 stylized effects). Pure-pixel kernel: no
 * asset deps, no time dep. All-zero shifts → identity
 * (early-out skips the per-pixel pass). Alpha is unmodified.
 *
 * Per-pixel:
 *   R = src[clamp(x + red_dx),  clamp(y + red_dy) ].R
 *   G = src[x, y].G
 *   B = src[clamp(x + blue_dx), clamp(y + blue_dy)].B
 *   A = src[x, y].A
 *
 * Source coordinates clamp at frame edges. Determinism:
 * pure integer indexing. Same shifts + same input bytes →
 * same output bytes across hosts.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0      → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4                  → ME_E_INVALID_ARG.
 *   - any shift component out of [-32, 32] px   → ME_E_INVALID_ARG.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_chromatic_aberration_inplace(
    std::uint8_t*                              rgba,
    int                                        width,
    int                                        height,
    std::size_t                                stride_bytes,
    const me::ChromaticAberrationEffectParams& params);

}  // namespace me::compose
