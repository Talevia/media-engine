/* `EffectKind::ChromaticAberration` typed parameters — M12
 * §156 (3/5 stylized effects).
 *
 * Per-channel pixel offset: read R from
 * (x + red_dx, y + red_dy), G from (x, y), B from
 * (x + blue_dx, y + blue_dy). Source-coord clamping at frame
 * edges keeps reads in-bounds.
 *
 * Default-constructed param is identity (all zero shifts);
 * the kernel recognizes this and skips the per-pixel pass. */
#pragma once

namespace me {

struct ChromaticAberrationEffectParams {
    /* Red-channel pixel offset, range [-32, 32]. */
    int red_dx  = 0;
    int red_dy  = 0;

    /* Blue-channel pixel offset, range [-32, 32]. */
    int blue_dx = 0;
    int blue_dy = 0;
};

}  // namespace me
