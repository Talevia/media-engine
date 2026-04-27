/*
 * me_engine_set_model_fetcher — M11 ml-model-lazy-load-callback.
 *
 * The whole TU is gated by `ME_HAS_INFERENCE`. When the engine is
 * built with `-DME_WITH_INFERENCE=OFF` the file compiles to nothing
 * (no symbols emitted), matching the header gate at
 * `include/media_engine/ml.h`.
 *
 * Pre-cycle-29 status: stub-but-functional — registration stores the
 * callback verbatim. The fetcher is never invoked by any current
 * code path. Follow-up M11 cycles wire the inference runtime that
 * consults the stored fetcher; license whitelist enforcement
 * happens THERE, not in this registration TU (per
 * `include/media_engine/ml.h`'s usage shape doc).
 */
#ifdef ME_HAS_INFERENCE

#include "media_engine/ml.h"
#include "core/engine_impl.hpp"

#include <mutex>

extern "C" me_status_t me_engine_set_model_fetcher(
    me_engine_t*        engine,
    me_model_fetcher_t  cb,
    void*               user) {
    if (!engine) return ME_E_INVALID_ARG;

    std::lock_guard<std::mutex> lk(engine->model_fetcher_mu);
    engine->model_fetcher_cb   = cb;
    engine->model_fetcher_user = user;
    return ME_OK;
}

#endif /* ME_HAS_INFERENCE */
