/* `me::compose::register_hue_saturation_kind` — register
 * `TaskKindId::RenderHueSaturation` with the global task
 * registry.
 *
 * M12 §155 (2/4 color effects). Sibling of tone_curve_stage
 * but uses three Float64 properties (hue_shift_deg,
 * saturation_scale, lightness_scale) instead of string-encoded
 * arrays.
 *
 * Inputs:  one RGBA8 frame.
 * Outputs: one RGBA8 frame with sRGB → HSL → adjust → HSL →
 *          sRGB applied per-pixel.
 *
 * Properties:
 *   - `hue_shift_deg`     (Float64) — degrees, defaults 0.0
 *   - `saturation_scale`  (Float64) — multiplicative, defaults 1.0
 *   - `lightness_scale`   (Float64) — multiplicative, defaults 1.0
 *
 * `time_invariant=true` — pure pixel transform.
 */
#pragma once

namespace me::compose {

void register_hue_saturation_kind();

}  // namespace me::compose
