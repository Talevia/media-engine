/* `EffectKind::RadialBlur` typed parameters — M12 §157
 * (2/3 blur variants).
 *
 * Zoom-style radial blur: for each output pixel, sample along
 * the line from the radial center through the pixel, scaled
 * over a small range around 1.0. Visually creates a
 * "speed-toward-center" impression (or "zoom-out" depending on
 * the sign convention; here intensity > 0 spreads taps both
 * inside and outside the pixel's current radius symmetrically).
 *
 * `center_*` are normalized [0, 1] (0 = top-left). Float here
 * is acceptable: it's converted to an integer pixel coord once
 * per kernel invocation (round-half-up), then the per-pixel loop
 * is pure integer math. VISION §3.1 byte-identity preserved
 * because the project compiles without `-ffast-math` and the
 * single-multiplication round happens at well-defined IEEE-754
 * boundaries.
 *
 * `intensity` is the scale spread: tap i scales the pixel-to-
 * center vector by (1 + (2i - (N-1)) * intensity / (N-1)).
 * intensity = 0 → identity; 0.05 = ±5% spread; max 1.0 = ±100%
 * (taps cover the full center-to-pixel line plus its reflection
 * through center). Stored as float; kernel converts to per-mille
 * integer once. */
#pragma once

namespace me {

struct RadialBlurEffectParams {
    /* Normalized center [0, 1]. Default = image center. */
    float center_x = 0.5f;
    float center_y = 0.5f;
    /* Scale spread around 1.0. Range [0, 1]; 0 = identity. */
    float intensity = 0.0f;
    /* Tap count along the radial line. Range [1, 64].
     * samples = 1 → identity. */
    int   samples   = 1;
};

}  // namespace me
