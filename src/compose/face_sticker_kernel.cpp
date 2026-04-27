/*
 * face_sticker_kernel impl — STUB. See header for the deferred-impl
 * rationale (`face-sticker-impl` follow-up bullet).
 *
 * The function is a typed-reject — any input + valid params returns
 * ME_E_UNSUPPORTED unconditionally. We still validate `rgba !=
 * nullptr` / dimensions / non-empty params to mirror the shape
 * callers expect from `apply_inverse_tonemap_inplace`; that lets
 * future cycles drop the impl in place without rewriting the
 * validation prologue. ME_E_INVALID_ARG takes precedence over
 * ME_E_UNSUPPORTED for argument-shape rejects so callers get the
 * right diagnostic axis.
 */
#include "compose/face_sticker_kernel.hpp"

namespace me::compose {

me_status_t apply_face_sticker_inplace(
    std::uint8_t*                          rgba,
    int                                    width,
    int                                    height,
    std::size_t                            stride_bytes,
    const me::FaceStickerEffectParams&     params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (params.landmark_asset_id.empty()) return ME_E_INVALID_ARG;
    if (params.sticker_uri.empty())       return ME_E_INVALID_ARG;

    /* STUB: face-sticker-impl — deferred until M11 inference runtime
     * + sticker decoder + affine compose stage land. Registered now
     * so JSON authoring tools can target the kind ahead of the impl. */
    return ME_E_UNSUPPORTED;
}

}  // namespace me::compose
