/*
 * tone_curve_kernel — per-channel piecewise-linear tone curve.
 *
 * M12 §155 (1/4 color effects). Pure-pixel kernel: builds three
 * 256-byte LUTs (one per RGB channel) from the control-point
 * lists in `me::ToneCurveEffectParams`, then applies them to
 * every pixel via byte indexing. Alpha is unmodified.
 *
 * Empty channel curves leave that channel unchanged (LUT[i]=i).
 * Curves with ≥ 2 points produce a piecewise-linear LUT
 * interpolated between adjacent points; values before the first
 * point's `x` map to `points[0].y`, values after the last
 * point's `x` map to `points[N-1].y`. Curves with exactly 1
 * point are rejected as malformed (loader guards this; the
 * kernel additionally rejects defensively).
 *
 * Determinism: pure integer LUT lookup; no float in the byte-
 * output path. Same params + same input bytes = same output
 * bytes across hosts. VISION §3.1 byte-identity holds.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0 → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4              → ME_E_INVALID_ARG.
 *   - any non-empty curve with size < 2     → ME_E_INVALID_ARG.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_tone_curve_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::ToneCurveEffectParams&       params);

}  // namespace me::compose
