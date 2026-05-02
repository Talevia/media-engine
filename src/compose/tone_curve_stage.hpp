/* `me::compose::register_tone_curve_kind` — register
 * `TaskKindId::RenderToneCurve` with the global task registry.
 *
 * M12 §155 (1/4 color effects). Sibling of face_mosaic_stage
 * but no asset dependency: pure pixel input + serialized control
 * points.
 *
 * Inputs:
 *   - one RGBA8 frame.
 *
 * Properties (control points serialized as 3 sentinel-prefixed
 * int64 streams, one per RGB channel):
 *   - `tone_curve_r_points` (string) — packed control points
 *     for R channel as ASCII "x0,y0;x1,y1;..." (uint8 pairs
 *     comma-separated, segments semicolon-separated). Empty
 *     string = no curve = identity for that channel.
 *   - `tone_curve_g_points` (string) — same for G.
 *   - `tone_curve_b_points` (string) — same for B.
 *
 * Why string-encoded pairs (instead of separate tasks per
 * point): graph::Properties variant doesn't support nested
 * arrays; the string-encoded packed representation is the
 * shortest path that preserves all curve data through the
 * graph properties layer. Loader serializes once at compile
 * time; kernel parses once at first invocation.
 *
 * Outputs:
 *   - one RGBA8 frame with the per-channel curves applied.
 *
 * `time_invariant=true`: same input + same params = same
 * output regardless of frame_t. The kernel is pure pixel-
 * arithmetic without any time dependency.
 */
#pragma once

namespace me::compose {

void register_tone_curve_kind();

}  // namespace me::compose
