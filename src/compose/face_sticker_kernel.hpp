/*
 * face_sticker_kernel — landmark-driven sticker overlay (M11 stub).
 *
 * Counterpart to `inverse_tonemap_kernel.hpp` in shape — the
 * registered-but-deferred pattern. The bullet's brief is "register
 * the kind, defer the impl, return ME_E_UNSUPPORTED with a clear
 * pointer to the follow-up bullet" — see `face-sticker-impl` in the
 * P2 backlog.
 *
 * Why deferred. The actual impl needs:
 *   1. An inference runtime that consumes the landmark asset (the
 *      M11 ml-runtime cycles, blocked on ML model fetcher landing
 *      from cycle 29 + actual CoreML / ONNX adapter).
 *   2. A sticker-decoder kernel (PNG / WebP probe via libavformat;
 *      shares decode path with `me_thumbnail_png` in spirit but on
 *      a pre-existing image asset rather than a video frame).
 *   3. A compose-graph stage that composites the sticker over the
 *      RGBA8 frame at the landmark anchor with affine
 *      (scale + offset) — overlaps with the existing
 *      `RenderAffineBlit` kernel but needs landmark-time-coupled
 *      params per-frame.
 *
 * None of those exist pre-cycle 30. The stub here preserves the
 * API surface so timeline JSON authoring tools can include
 * `kind: "face_sticker"` ahead of the impl. Same registered-but-
 * deferred pattern as `inverse_tonemap` was pre-cycle 24.
 *
 * Determinism on the stub. The function unconditionally returns
 * ME_E_UNSUPPORTED for any input — that's a deterministic answer.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Always returns ME_E_UNSUPPORTED today. Documented at the kind
 * registration point (timeline_ir_params.hpp:EffectKind::FaceSticker)
 * and the loader dispatch (loader_helpers_clip_params.cpp). When
 * the `face-sticker-impl` bullet lands, this signature stays — only
 * the body changes. The kernel will gain extra parameters for the
 * resolved landmark stream + sticker pixel buffer at impl time;
 * those can be appended without breaking call sites by making them
 * optional or routing through the graph::EvalContext.
 *
 * Argument-shape rejects (null buffer / non-positive dims / undersized
 * stride / empty landmark_asset_id / empty sticker_uri) take
 * precedence over the UNSUPPORTED short-circuit so a future impl can
 * drop in without rewriting the prologue. */
me_status_t apply_face_sticker_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceStickerEffectParams&     params);

}  // namespace me::compose
