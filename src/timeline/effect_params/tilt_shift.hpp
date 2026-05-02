/* `EffectKind::TiltShift` typed parameters — M12 §157
 * (3/3 blur variants).
 *
 * Tilt-shift simulates a wide-aperture lens with a narrow
 * depth-of-field band: rows whose normalized y is inside
 * [focal_y_min, focal_y_max] stay sharp; rows outside ramp up
 * to `max_blur_radius` over `edge_softness` (normalized).
 *
 * Per-row blur radius:
 *   if focal_y_min <= yn <= focal_y_max:
 *     r(y) = 0
 *   else:
 *     d = (yn < focal_y_min) ? (focal_y_min - yn) : (yn - focal_y_max)
 *     t = clamp(d / edge_softness, 0, 1)
 *     r(y) = round(t * max_blur_radius)
 *
 * The radius is then used for an isotropic (2r+1)×(2r+1) box
 * average centered on each pixel in that row. Floats are
 * converted to per-mille integers once on entry; per-pixel hot
 * loop is integer-only.
 *
 * focal_y_min must be ≤ focal_y_max, both in [0, 1].
 * edge_softness in (0, 1] (must be > 0; 0 would div-by-zero).
 * max_blur_radius in [0, 32] (0 = identity). */
#pragma once

namespace me {

struct TiltShiftEffectParams {
    /* In-focus band [min, max], normalized [0, 1]. Default
     * = full-image-in-focus identity-like band centered. */
    float focal_y_min     = 0.4f;
    float focal_y_max     = 0.6f;
    /* Soft-edge ramp width, normalized [0, 1]. Must be > 0. */
    float edge_softness   = 0.2f;
    /* Maximum blur radius outside the band, in pixels. Range
     * [0, 32]. 0 = no blur (identity). */
    int   max_blur_radius = 0;
};

}  // namespace me
