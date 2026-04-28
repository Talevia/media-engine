/*
 * body_alpha_key_kernel — mask-driven alpha key.
 *
 * Post-`body-alpha-key-impl`: the kernel does the deterministic
 * byte-level math given a **pre-resolved** per-frame alpha mask.
 * Upstream mask-asset resolution (running portrait segmentation
 * + producing an 8-bit alpha plane sized to the frame) lives in
 * the future compose-stage wiring (the same resolver shape that
 * drives face_sticker / face_mosaic, tracked under
 * `face-sticker-compose-stage-wiring`).
 *
 * Math:
 *   1. Optional invert: m' = invert ? 255 - m : m.
 *   2. Optional feather: box-blur m' by `feather_radius_px`
 *      pixels on each axis (two-pass separable, deterministic).
 *   3. Per-pixel: `output.alpha = (input.alpha * m'_blurred +
 *      127) / 255`, RGB unchanged.
 *
 * Determinism: pure CPU integer arithmetic with /255 round-half-
 * up; same inputs produce same bytes across hosts. No SIMD, no
 * parallelism.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Apply the mask as the foreground alpha key. The mask buffer
 * must have the same dimensions as the frame (`mask_width ==
 * width && mask_height == height`); a future cycle can add
 * resampling for differently-sized masks if needed.
 *
 * `params.mask_asset_id` is documentation-only at the kernel
 * level (the upstream resolver consumes it). The kernel uses
 * `params.feather_radius_px` (≥ 0) and `params.invert`.
 *
 * Empty / null mask → ME_OK no-op (resolver may legitimately
 * produce no mask for a frame).
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0          → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4                       → ME_E_INVALID_ARG.
 *   - mask_stride < mask_width (when mask non-null)  → ME_E_INVALID_ARG.
 *   - feather_radius_px < 0                          → ME_E_INVALID_ARG.
 *   - mask dimensions ≠ frame dimensions             → ME_E_INVALID_ARG. */
me_status_t apply_body_alpha_key_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::BodyAlphaKeyEffectParams&    params,
    const std::uint8_t*                    mask,
    int                                    mask_width,
    int                                    mask_height,
    std::size_t                            mask_stride);

}  // namespace me::compose
