/*
 * glitch_kernel — deterministic horizontal block stripes
 * shifted by a per-row PRNG offset, with optional per-channel
 * (R/B) shift on top.
 *
 * M12 §156 (1/5 stylized effects). Pure-pixel kernel: no
 * asset deps, no time dep. Identity intensity (0.0)
 * early-out skips the per-pixel pass; alpha is unmodified.
 *
 * Algorithm:
 *   For each block_y stripe (block height = block_size_px):
 *     1. Hash (seed, block_y) via FNV-1a + xorshift64.
 *     2. Map xorshift output to a signed pixel-shift in
 *        [-max_shift, +max_shift] where max_shift =
 *        round(intensity * width).
 *     3. Per-channel R/B additional shift in
 *        [-channel_shift_max_px, +channel_shift_max_px]
 *        sourced from a second xorshift step.
 *   For each pixel (x, y) in the block:
 *     - read source pixel at (clamp(x - block_shift), y)
 *     - R = src[clamp(srcx + r_shift)].R
 *     - G = src[srcx].G
 *     - B = src[clamp(srcx + b_shift)].B
 *     - alpha unchanged from src[srcx].
 *
 * Determinism: pure integer math (FNV-1a + xorshift64); same
 * (seed, block_y, x) → same output bytes across hosts.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0    → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4                → ME_E_INVALID_ARG.
 *   - non-finite intensity                    → ME_E_INVALID_ARG.
 *   - block_size_px < 1 OR > 64               → ME_E_INVALID_ARG.
 *   - channel_shift_max_px < 0 OR > 16        → ME_E_INVALID_ARG.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_glitch_inplace(
    std::uint8_t*                 rgba,
    int                           width,
    int                           height,
    std::size_t                   stride_bytes,
    const me::GlitchEffectParams& params);

}  // namespace me::compose
