/*
 * body_alpha_key_kernel — mask-driven alpha key (M11 stub).
 *
 * Counterpart to face_sticker_kernel and face_mosaic_kernel —
 * registered-but-deferred per `body-alpha-key-impl` follow-up.
 * Applies a portrait segmentation mask (Asset.kind ==
 * AssetKind::Mask) as the foreground alpha channel: where the
 * mask says "background" the output goes transparent; where it
 * says "foreground" the output carries the input.
 *
 * Why deferred. Same upstream wiring need as face_sticker /
 * face_mosaic: an inference runtime that resolves the Mask asset
 * to a per-frame alpha buffer + a compose-graph stage that pipes
 * that buffer into the kernel. Once the buffer is available, the
 * kernel is straightforward deterministic byte-arithmetic
 * (alpha = mask × input.alpha; optional invert + box-blur feather).
 *
 * Determinism on the stub. Returns ME_E_UNSUPPORTED for any
 * input — deterministic answer. The future impl is explicitly
 * deterministic — VISION §3.1 byte-identity holds.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Always returns ME_E_UNSUPPORTED today. Documented at the kind
 * registration point (timeline_ir_params.hpp:EffectKind::BodyAlphaKey)
 * and the loader dispatch (loader_helpers_clip_params.cpp). When
 * the `body-alpha-key-impl` bullet lands, this signature stays —
 * only the body changes (likely gains a `const Mask& mask` arg via
 * graph::EvalContext). Argument-shape rejects (null buffer / non-
 * positive dims / undersized stride / empty mask_asset_id /
 * negative feather_radius_px) take precedence over the UNSUPPORTED
 * short-circuit so a future impl can drop in without rewriting
 * the prologue. */
me_status_t apply_body_alpha_key_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::BodyAlphaKeyEffectParams&    params);

}  // namespace me::compose
