/* `EffectKind::HueSaturation` typed parameters — M12 §155
 * (2/4 color effects).
 *
 * Per-pixel sRGB → HSL → adjust → HSL → sRGB transform.
 * `hue_shift_deg` rotates the hue angle (wraps modulo 360°);
 * `saturation_scale` and `lightness_scale` are multiplicative
 * (1.0 = identity, 0.0 = desaturate / black, 2.0 = double S/L
 * with clamping at 1.0 in the HSL domain).
 *
 * Default-constructed param is identity (hue_shift_deg=0,
 * saturation_scale=1, lightness_scale=1) — the kernel
 * recognizes this and skips the per-pixel transform entirely. */
#pragma once

namespace me {

struct HueSaturationEffectParams {
    /* Hue rotation in degrees. Wraps modulo 360°. Range
     * conventionally [-180, 180] but any float value is
     * accepted; the kernel takes `fmod(h + shift, 360)` so
     * out-of-range values still produce a sane result. */
    float hue_shift_deg    = 0.0f;

    /* Multiplicative saturation scale. 1.0 = identity; 0.0 =
     * fully desaturate (grey); >1.0 oversaturates with HSL-S
     * clamped to 1.0. Negative values clamp to 0. */
    float saturation_scale = 1.0f;

    /* Multiplicative lightness scale. 1.0 = identity; 0.0 =
     * black; >1.0 brightens with HSL-L clamped to 1.0.
     * Negative values clamp to 0. */
    float lightness_scale  = 1.0f;
};

}  // namespace me
