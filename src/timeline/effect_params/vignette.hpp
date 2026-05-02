/* `EffectKind::Vignette` typed parameters — M12 §155
 * (3/4 color effects).
 *
 * Radial darkening around a configurable center. Falloff is a
 * smoothstep curve from `radius` (full intensity inside) to
 * `radius + softness` (no intensity outside the soft band);
 * `intensity` controls the depth of the dark ring (0 = no
 * vignette / identity, 1 = full black at the edge).
 *
 * Default-constructed param is identity (intensity=0); the
 * kernel recognizes this and skips the per-pixel transform. */
#pragma once

namespace me {

struct VignetteEffectParams {
    /* Inner radius of the vignette band (normalized to half the
     * smaller frame dimension; 0 = center, 1 = edge of the
     * smaller dim). Pixels inside `radius` are unmodified. */
    float radius    = 0.5f;

    /* Width of the smoothstep falloff band (normalized same way
     * as `radius`). Pixels at distance >= `radius + softness`
     * receive the full `intensity` darkening. softness=0
     * produces a hard edge. */
    float softness  = 0.3f;

    /* Vignette depth. 0.0 = no effect (identity); 1.0 = pixels
     * past the soft band are pure black. Negative values clamp
     * to 0. */
    float intensity = 0.0f;

    /* Center point in normalized [0, 1]² coordinates. Default
     * (0.5, 0.5) = frame center. */
    float center_x  = 0.5f;
    float center_y  = 0.5f;
};

}  // namespace me
