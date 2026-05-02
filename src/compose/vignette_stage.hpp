/* `me::compose::register_vignette_kind` — register
 * `TaskKindId::RenderVignette` with the global task registry.
 *
 * M12 §155 (3/4 color effects). Sibling of
 * hue_saturation_stage; uses five Float64 properties.
 *
 * Inputs:  one RGBA8 frame.
 * Outputs: one RGBA8 frame with radial darkening applied.
 *
 * Properties:
 *   - `radius`    (Float64) — normalized inner radius, default 0.5
 *   - `softness`  (Float64) — normalized soft band width, default 0.3
 *   - `intensity` (Float64) — 0..1, default 0 (identity)
 *   - `center_x`  (Float64) — 0..1 normalized, default 0.5
 *   - `center_y`  (Float64) — 0..1 normalized, default 0.5
 *
 * `time_invariant=true`.
 */
#pragma once

namespace me::compose {

void register_vignette_kind();

}  // namespace me::compose
