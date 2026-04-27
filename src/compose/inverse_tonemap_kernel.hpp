/*
 * inverse_tonemap_kernel — SDR → HDR expansion stub.
 *
 * Counterpart to `tonemap_kernel.{hpp,cpp}` (M10 exit criterion 6
 * second half — `inverse-tonemap-effect-stub`). The bullet's brief
 * is "register the kind, defer the impl, return ME_E_UNSUPPORTED
 * with a clear pointer to the follow-up bullet" — see
 * `inverse-tonemap-effect-impl` in the P2 backlog.
 *
 * Why deferred. Inverse tonemap fundamentally INVENTS information
 * the SDR input doesn't contain (specular highlights collapsed
 * onto byte 255, deep shadow texture crushed into the bottom
 * octave). Real implementations (HDR boost, Dolby Content Mapping,
 * DeepHDR neural networks) are non-deterministic by construction —
 * output depends on GPU non-determinism, model weights at the time
 * of call, etc. VISION §3.1's deterministic-software-path contract
 * forbids landing such an op into the default chain. The stub here
 * preserves the API surface so timeline JSON authoring tools can
 * include `kind: "inverse_tonemap"` ahead of the impl.
 *
 * Determinism on the stub. The function unconditionally returns
 * ME_E_UNSUPPORTED for any input — that's a deterministic answer.
 * The comment-level diag (no err out param to match
 * `apply_tonemap_inplace`'s signature) names the follow-up bullet
 * so callers grepping for it land on the impl tracker.
 */
#pragma once

#include "timeline/timeline_ir_params.hpp"

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Always returns ME_E_UNSUPPORTED today. Documented at the kind
 * registration point (timeline_ir_params.hpp) and the loader
 * dispatch (loader_helpers_clip_params.cpp). When the
 * `inverse-tonemap-effect-impl` bullet lands, this signature stays
 * — only the body changes. */
me_status_t apply_inverse_tonemap_inplace(
    std::uint8_t*                              rgba,
    int                                        width,
    int                                        height,
    std::size_t                                stride_bytes,
    const me::InverseTonemapEffectParams&      params);

}  // namespace me::compose
