/* `EffectKind::MotionBlur` typed parameters — M12 §157
 * (1/3 blur variants).
 *
 * Linear-motion blur: each output pixel is the average of
 * `samples` reads taken along an integer-pixel direction
 * vector centered on the pixel. Direction stored as integer
 * pixel offsets `dx_px` / `dy_px` rather than (length, angle)
 * so the kernel can advance in pure integer math (no sin /
 * cos / FP rounding) — VISION §3.1 byte-identity friendly.
 *
 * Tap positions: i ∈ {-(samples-1)/2 .. +(samples/2)} (zero-
 * centered). Coordinates clamp at image edges. samples=1
 * degenerates to identity (single read at the source pixel).
 *
 * Future improvement (out of scope): derive (dx_px, dy_px)
 * from clip transform velocity at `t`. */
#pragma once

namespace me {

struct MotionBlurEffectParams {
    /* Total blur vector across the line (pixels, signed). */
    int dx_px   = 0;
    int dy_px   = 0;
    /* Tap count along the line. Range [1, 64]. samples=1 =
     * identity (no-op). */
    int samples = 1;
};

}  // namespace me
