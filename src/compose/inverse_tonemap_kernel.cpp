/*
 * inverse_tonemap_kernel impl — STUB. See header for the deferred-
 * impl rationale (`inverse-tonemap-effect-impl`).
 *
 * The function is a typed-reject — any input + params returns
 * ME_E_UNSUPPORTED unconditionally. We still validate `rgba !=
 * nullptr` and dimensions to mirror the shape callers expect from
 * `apply_tonemap_inplace`; that lets future cycles drop the impl in
 * place without rewriting the validation prologue. ME_E_INVALID_ARG
 * takes precedence over ME_E_UNSUPPORTED for argument-shape rejects
 * so callers get the right diagnostic axis.
 *
 * Why no err out param: matches `apply_tonemap_inplace`'s minimal
 * signature. The "diag" lives in code comments + the bullet slug
 * referenced in the header — `git log --grep=inverse-tonemap` /
 * `git log -S 'inverse_tonemap'` walks straight to the impl tracker
 * when it lands.
 */
#include "compose/inverse_tonemap_kernel.hpp"

namespace me::compose {

me_status_t apply_inverse_tonemap_inplace(
    std::uint8_t*                              rgba,
    int                                        width,
    int                                        height,
    std::size_t                                stride_bytes,
    const me::InverseTonemapEffectParams&      params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        return ME_E_INVALID_ARG;
    }
    if (!(params.target_peak_nits > 0.0)) return ME_E_INVALID_ARG;

    // STUB: inverse-tonemap-effect-impl — backlog P2 bullet defines
    // the deferred SDR→HDR expansion. Today the kind is registered +
    // parsed but the kernel intentionally returns UNSUPPORTED so
    // hosts can compile JSON against the schema without accidentally
    // shipping a non-deterministic op into the default chain.
    return ME_E_UNSUPPORTED;
}

}  // namespace me::compose
