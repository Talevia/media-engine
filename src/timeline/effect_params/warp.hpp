/* `EffectKind::Warp` typed parameters — M12 §158 (1/2
 * geometric effects).
 *
 * Free-form mesh warp driven by a list of control points.
 * Each control point declares a source-to-destination mapping:
 * "the input pixel at (src_x, src_y) appears at output position
 * (dst_x, dst_y)". The kernel reconstructs an inverse-mapping
 * function over the whole image via inverse-distance-weighted
 * (IDW) interpolation between the control points, then samples
 * the input bilinearly at each output pixel's source position.
 *
 * Coordinates are normalized [0, 1] so the same control set
 * can drive frames of different resolution.
 *
 * Empty control_points list, or all src == dst, → identity
 * (kernel early-out).
 *
 * Float-determinism caveat: like vignette and hue_saturation,
 * the kernel's IDW step uses IEEE-754 double-precision arith.
 * The project compiles without -ffast-math; same bytes on the
 * same compiler+arch combination. Cross-compiler bit equality
 * is not guaranteed but matches the precedent for "complex"
 * effects in this codebase. */
#pragma once

#include <vector>

namespace me {

struct WarpControlPoint {
    /* Source position in input image (normalized). The pixel at
     * this position should appear at (dst_x, dst_y) in output. */
    float src_x = 0.0f;
    float src_y = 0.0f;
    float dst_x = 0.0f;
    float dst_y = 0.0f;
};

struct WarpEffectParams {
    /* 0..32 control points. Empty list = identity (no-op). */
    std::vector<WarpControlPoint> control_points;
};

}  // namespace me
