/*
 * ordered_dither_kernel — Bayer-matrix dither + per-channel
 * quantization.
 *
 * M12 §156 (5/5 stylized effects). Pure-pixel kernel. The
 * threshold offset comes from a documented Bayer matrix
 * (2×2, 4×4, or 8×8). Per-pixel:
 *
 *   threshold_norm = (bayer[y % size][x % size] + 0.5) /
 *                    (size * size)        // ∈ [1/N², 1)
 *   in_with_dither = in + (threshold_norm - 0.5) * step
 *   bucket         = round(in_with_dither * (levels-1) / 255)
 *   out            = round(bucket * 255 / (levels-1))
 *
 * where `step = 255 / (levels - 1)` is the quantum width.
 * Adding the dither offset *before* quantization breaks
 * banded gradients into a periodic stipple pattern.
 *
 * Determinism: pure integer math (Bayer values stored as
 * scaled-integer thresholds; quantization is the same round-
 * half-up integer divide as posterize).
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / w/h <= 0           → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4             → ME_E_INVALID_ARG
 *   - matrix_size not in {2, 4, 8}         → ME_E_INVALID_ARG
 *   - levels < 2 OR levels > 256           → ME_E_INVALID_ARG
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_ordered_dither_inplace(
    std::uint8_t*                        rgba,
    int                                  width,
    int                                  height,
    std::size_t                          stride_bytes,
    const me::OrderedDitherEffectParams& params);

}  // namespace me::compose
