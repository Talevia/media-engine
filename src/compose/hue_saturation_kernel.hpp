/*
 * hue_saturation_kernel — sRGB → HSL → adjust → HSL → sRGB
 * per-pixel transform.
 *
 * M12 §155 (2/4 color effects). Pure-pixel kernel: no asset
 * deps, no time dep. Identity params (hue_shift_deg=0,
 * saturation_scale=1, lightness_scale=1) early-out before any
 * per-pixel math; alpha is unmodified by all paths.
 *
 * Determinism. The transform uses single-precision float
 * internally for the HSL math (max/min/divide for the
 * conversions). On all current targets (clang/GCC, no
 * -ffast-math, no FMA contract) IEEE-754 single-precision
 * deterministically reproduces across hosts. The byte-output
 * path quantizes via `int(f * 255 + 0.5)` clamped to [0, 255]
 * — round-half-up consistent with the project's other byte-
 * domain kernels (face_mosaic Pixelate, tone_curve LUT). If
 * a future cycle introduces -ffast-math or SIMD-reordered
 * loops that break the determinism, the
 * `debt-hue-saturation-fixed-point-impl` follow-up bullet
 * tracks the fixed-point migration.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0 → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4              → ME_E_INVALID_ARG.
 *   - non-finite scale params               → ME_E_INVALID_ARG.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

me_status_t apply_hue_saturation_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::HueSaturationEffectParams&   params);

}  // namespace me::compose
