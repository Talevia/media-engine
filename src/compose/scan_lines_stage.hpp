/* `me::compose::register_scan_lines_kind` — register
 * `TaskKindId::RenderScanLines` with the global task registry.
 *
 * M12 §156 (2/5 stylized effects). Three properties:
 *   - `line_height_px`  (Int64)   — 1..64, default 2
 *   - `darkness`        (Float64) — 0..1, default 0 (identity)
 *   - `phase_offset_px` (Int64)   — 0..63, default 0
 *
 * `time_invariant=true`.
 */
#pragma once

namespace me::compose {

void register_scan_lines_kind();

}  // namespace me::compose
