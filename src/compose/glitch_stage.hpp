/* `me::compose::register_glitch_kind` — register
 * `TaskKindId::RenderGlitch` with the global task registry.
 *
 * M12 §156 (1/5 stylized effects). Sibling of
 * film_grain_stage; uses Int64 for `seed` + Float64 for
 * `intensity` + Int64 for the two pixel-count params.
 *
 * Inputs:  one RGBA8 frame.
 * Outputs: one RGBA8 frame with horizontal block stripes
 *          shifted by a per-row PRNG offset.
 *
 * Properties:
 *   - `seed`                  (Int64)   — PRNG seed
 *   - `intensity`             (Float64) — 0..1, default 0
 *   - `block_size_px`         (Int64)   — 1..64, default 8
 *   - `channel_shift_max_px`  (Int64)   — 0..16, default 0
 *
 * `time_invariant=true` — same as film_grain.
 */
#pragma once

namespace me::compose {

void register_glitch_kind();

}  // namespace me::compose
