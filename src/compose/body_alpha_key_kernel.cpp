/*
 * body_alpha_key_kernel impl — STUB. See header for the deferred-impl
 * rationale (`body-alpha-key-impl` follow-up bullet).
 *
 * Argument-shape rejects (null / non-positive dims / undersized
 * stride / empty mask_asset_id / negative feather_radius_px) land
 * before the UNSUPPORTED short-circuit so a future impl can drop in
 * without rewriting the prologue.
 */
#include "compose/body_alpha_key_kernel.hpp"

namespace me::compose {

me_status_t apply_body_alpha_key_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::BodyAlphaKeyEffectParams&    params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (params.mask_asset_id.empty())     return ME_E_INVALID_ARG;
    if (params.feather_radius_px < 0)     return ME_E_INVALID_ARG;

    /* STUB: body-alpha-key-impl — deferred until M11 inference
     * runtime resolves the Mask asset + per-frame alpha buffer lands. */
    return ME_E_UNSUPPORTED;
}

}  // namespace me::compose
