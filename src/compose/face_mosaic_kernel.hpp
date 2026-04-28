/*
 * face_mosaic_kernel — landmark-driven privacy mosaic / blur.
 *
 * Post-`face-mosaic-impl`: the kernel does the deterministic
 * byte-level math given **pre-resolved** bbox inputs. Upstream
 * landmark-stream resolution (running BlazeFace + projecting
 * landmarks → `me::compose::Bbox` per face) lives in the future
 * compose-stage wiring (`face-sticker-compose-stage-wiring` —
 * the same resolver shape drives face_mosaic).
 *
 * Two modes (`FaceMosaicEffectParams::Kind`):
 *   - Pixelate: tile the bbox in `block_size_px × block_size_px`
 *     squares, replace every pixel in a tile with the per-channel
 *     mean of that tile. Mosaic look.
 *   - Blur: box-filter pass at radius `block_size_px / 2` over
 *     the bbox region. Smoother privacy mask.
 *
 * Determinism. Pure CPU byte-arithmetic with /(N) round-half-up
 * fixed-point — same input bytes produce the same output bytes
 * across hosts. No SIMD, no parallelism, no rand.
 */
#pragma once

#include "compose/bbox.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace me::compose {

/* Apply the configured mosaic mode within each `bbox` in
 * `landmark_bboxes`. Empty bboxes / bbox out-of-bounds → no-op
 * (resolver may legitimately produce empty boxes for low-
 * confidence frames). Bboxes outside the image are clamped to
 * the image extent before processing.
 *
 * `params.landmark_asset_id` is documentation-only at the
 * kernel level (the upstream resolver consumes it). The kernel
 * uses `params.block_size_px` (must be > 0) and `params.kind`.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0 → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4              → ME_E_INVALID_ARG.
 *   - block_size_px <= 0                    → ME_E_INVALID_ARG. */
me_status_t apply_face_mosaic_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceMosaicEffectParams&      params,
    std::span<const Bbox>                  landmark_bboxes);

}  // namespace me::compose
