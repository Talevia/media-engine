/*
 * face_mosaic_kernel impl — STUB. See header for the deferred-impl
 * rationale (`face-mosaic-impl` follow-up bullet).
 *
 * Argument-shape rejects (null / non-positive dims / undersized
 * stride / empty landmark_asset_id / non-positive block_size_px)
 * land before the UNSUPPORTED short-circuit so a future impl can
 * drop in without rewriting the prologue.
 */
#include "compose/face_mosaic_kernel.hpp"

namespace me::compose {

me_status_t apply_face_mosaic_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceMosaicEffectParams&      params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (params.landmark_asset_id.empty())   return ME_E_INVALID_ARG;
    if (params.block_size_px <= 0)          return ME_E_INVALID_ARG;

    /* STUB: face-mosaic-impl — deferred until M11 inference runtime
     * resolves the landmark stream + per-frame bbox computation lands. */
    return ME_E_UNSUPPORTED;
}

}  // namespace me::compose
