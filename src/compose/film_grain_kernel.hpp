/*
 * film_grain_kernel — additive deterministic noise per pixel.
 *
 * M12 §155 (4/4 color effects). Pure-pixel kernel: no asset
 * deps, no time dep (the kernel doesn't take frame_t — for
 * shot-stable grain the host picks a fixed `seed`; for
 * per-frame variation the host can keyframe `seed` via the
 * future animated-param path). Identity amount (0.0)
 * early-out skips the per-pixel pass; alpha is unmodified.
 *
 * Determinism. The PRNG is xorshift64 seeded from
 * `mix(params.seed, block_y, block_x)` where
 * `(block_x, block_y) = (x / grain_size, y / grain_size)`.
 * Each grain block computes one signed delta in [-127, 127]
 * and applies it identically to all pixels in the block. The
 * mixing function is documented in the .cpp file. Same seed
 * + same coordinates = same delta bytes across hosts (no
 * float, no SIMD reorder). VISION §3.1 byte-identity holds.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0 → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4              → ME_E_INVALID_ARG.
 *   - non-finite amount                     → ME_E_INVALID_ARG.
 *   - grain_size_px < 1 OR > 8              → ME_E_INVALID_ARG.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_film_grain_inplace(
    std::uint8_t*                      rgba,
    int                                width,
    int                                height,
    std::size_t                        stride_bytes,
    const me::FilmGrainEffectParams&   params);

}  // namespace me::compose
