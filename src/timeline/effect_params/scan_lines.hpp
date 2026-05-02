/* `EffectKind::ScanLines` typed parameters — M12 §156
 * (2/5 stylized effects).
 *
 * CRT-style horizontal scan lines: every Nth row is darkened
 * by `darkness` (multiplicative). Pure deterministic per-row
 * scan; no PRNG needed.
 *
 * Default-constructed param is identity (darkness=0); the
 * kernel recognizes this and skips the per-pixel transform. */
#pragma once

namespace me {

struct ScanLinesEffectParams {
    /* Row period in pixels. Every `line_height_px`-th row
     * (starting from `phase_offset_px`) is darkened. Range
     * [1, 64]. */
    int   line_height_px = 2;

    /* Darkening factor. 0.0 = no effect, 1.0 = darkened
     * rows are pure black. Negative clamps to 0; values >1
     * saturate at 1.0. */
    float darkness = 0.0f;

    /* Phase offset for which rows are darkened. The
     * darkened rows are at indices
     * `phase_offset_px, phase_offset_px + line_height_px,
     * phase_offset_px + 2 * line_height_px, ...`. Range
     * [0, 63]; the kernel takes `% line_height_px`. */
    int   phase_offset_px = 0;
};

}  // namespace me
