/*
 * face_mosaic_kernel — landmark-driven privacy mosaic / blur (M11 stub).
 *
 * Counterpart to face_sticker_kernel — registered-but-deferred per
 * `face-mosaic-impl` follow-up. Applies a per-block mosaic
 * (Pixelate: mean × replicate within block; Blur: box filter) to
 * the landmark's bounding-box region.
 *
 * Why deferred. Same prerequisites as face_sticker_kernel (see that
 * header for the full list): inference runtime + landmark stream
 * resolution + per-frame bbox computation. The mosaic / blur math
 * itself is deterministic CPU byte-arithmetic — once the bbox is
 * known, the kernel is a straightforward 2-pass loop. The block
 * here is upstream wiring, not the algorithm.
 *
 * Determinism on the stub. Returns ME_E_UNSUPPORTED for any input
 * — deterministic answer. The future impl is explicitly
 * deterministic (no randomness, no parallelism, no SIMD), so VISION
 * §3.1 byte-identity holds.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Always returns ME_E_UNSUPPORTED today. Documented at the kind
 * registration point (timeline_ir_params.hpp:EffectKind::FaceMosaic)
 * and the loader dispatch (loader_helpers_clip_params.cpp). When
 * the `face-mosaic-impl` bullet lands, this signature stays — only
 * the body changes (likely gains a `const Landmark& bbox` arg via
 * graph::EvalContext). Argument-shape rejects (null buffer / non-
 * positive dims / undersized stride / empty landmark_asset_id /
 * non-positive block_size_px) take precedence over the UNSUPPORTED
 * short-circuit so a future impl can drop in without rewriting the
 * prologue. */
me_status_t apply_face_mosaic_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceMosaicEffectParams&      params);

}  // namespace me::compose
