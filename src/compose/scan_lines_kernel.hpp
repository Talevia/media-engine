/*
 * scan_lines_kernel — CRT-style horizontal scan-line darkening.
 *
 * M12 §156 (2/5 stylized effects). Pure-pixel kernel: no
 * asset deps, no time dep, no PRNG. Identity darkness (0.0)
 * early-out skips the per-pixel pass; alpha is unmodified.
 *
 * Algorithm: every Nth row (where N = line_height_px,
 * starting from row index `phase_offset_px % line_height_px`)
 * has its RGB channels multiplied by `(1 - darkness)`. Other
 * rows are untouched.
 *
 * Determinism: pure integer math (round-half-up scaled-int
 * multiply); no float in the byte-output path. Same inputs
 * → same output bytes across hosts.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0    → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4                → ME_E_INVALID_ARG.
 *   - non-finite darkness                     → ME_E_INVALID_ARG.
 *   - line_height_px < 1 OR > 64              → ME_E_INVALID_ARG.
 *   - phase_offset_px < 0 OR >= 64            → ME_E_INVALID_ARG.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_scan_lines_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::ScanLinesEffectParams& params);

}  // namespace me::compose
