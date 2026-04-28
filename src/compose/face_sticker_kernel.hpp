/*
 * face_sticker_kernel — landmark-driven sticker overlay.
 *
 * As of `face-sticker-impl` (cycle 11+, post-CoreML-runtime
 * landing): the kernel does the deterministic byte-level math
 * given **pre-resolved** inputs — a list of per-frame face
 * bounding boxes (the landmark resolver's output) and the
 * already-decoded sticker pixel buffer (the sticker decoder's
 * output). Upstream layers handle:
 *
 *   1. Inference resolver — runs the landmark model
 *      (BlazeFace, etc.) on the source frame and projects
 *      landmarks → `me::compose::Bbox` per face. Tracked
 *      separately under the future
 *      `face-mosaic-resolver-wiring` cycle (the same resolver
 *      drives face_mosaic and face_sticker).
 *   2. Sticker decoder — decodes the `sticker_uri` (PNG / WebP)
 *      to RGBA8 once at compose-stage init. The decoder kernel
 *      is a sibling of the existing `me_thumbnail_png` decode
 *      path. Tracked under `face-sticker-decoder-wiring`.
 *
 * Determinism. Pure IEEE-754 float32 affine arithmetic + nearest-
 * neighbor sampling via `me::compose::affine_blit`. Same inputs
 * produce the same bytes across hosts.
 */
#pragma once

#include "compose/bbox.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace me::compose {

/* Composite the sticker over `rgba` at each `bbox` in
 * `landmark_bboxes`. The sticker is scaled to (`bbox.width *
 * params.scale_x`, `bbox.height * params.scale_y`), translated
 * by (`params.offset_x`, `params.offset_y`) from the bbox center,
 * and alpha-blended over the existing pixels (RGBA8 source-over).
 *
 * Empty inputs are no-ops:
 *   - `landmark_bboxes` empty → ME_OK, frame untouched.
 *   - `sticker_rgba == nullptr` or `sticker_w / sticker_h <= 0`
 *     → ME_OK, frame untouched.
 *
 * `params.landmark_asset_id` and `params.sticker_uri` are
 * documentation-only at the kernel level — they tell the
 * upstream resolver where to fetch landmark/sticker data; the
 * kernel itself only consumes `params.scale_x/y` and
 * `params.offset_x/y`.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr OR width/height <= 0 → ME_E_INVALID_ARG.
 *   - stride_bytes < width * 4 → ME_E_INVALID_ARG.
 *   - sticker_stride_bytes < sticker_w * 4 (when sticker_rgba
 *     non-null) → ME_E_INVALID_ARG. */
me_status_t apply_face_sticker_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceStickerEffectParams&     params,
    std::span<const Bbox>                  landmark_bboxes,
    const std::uint8_t*                    sticker_rgba,
    int                                    sticker_width,
    int                                    sticker_height,
    std::size_t                            sticker_stride_bytes);

}  // namespace me::compose
