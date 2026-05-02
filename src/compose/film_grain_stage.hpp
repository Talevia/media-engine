/* `me::compose::register_film_grain_kind` — register
 * `TaskKindId::RenderFilmGrain` with the global task registry.
 *
 * M12 §155 (4/4 color effects). Sibling of vignette_stage;
 * uses Int64 for `seed` (seed is uint64 cast through the
 * graph::Properties Int64 slot — no precision loss for
 * full uint64 range up to 2^63 - 1).
 *
 * Inputs:  one RGBA8 frame.
 * Outputs: one RGBA8 frame with deterministic per-pixel
 *          additive noise.
 *
 * Properties:
 *   - `seed`           (Int64)   — PRNG seed (cast to uint64)
 *   - `amount`         (Float64) — 0..1, default 0 (identity)
 *   - `grain_size_px`  (Int64)   — 1..8, default 1
 *
 * `time_invariant=true` — the kernel doesn't read frame_t.
 * For per-frame variation, the host keyframes `seed` (future
 * animated-param wiring).
 */
#pragma once

namespace me::compose {

void register_film_grain_kind();

}  // namespace me::compose
