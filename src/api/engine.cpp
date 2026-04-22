#include "media_engine/engine.h"
#include "core/engine_impl.hpp"

#include <new>

extern "C" me_status_t me_engine_create(const me_engine_config_t* config, me_engine_t** out) {
    if (!out) return ME_E_INVALID_ARG;
    auto* e = new (std::nothrow) me_engine{};
    if (!e) return ME_E_OUT_OF_MEMORY;
    if (config) e->config = *config;
    *out = e;
    return ME_OK;
}

extern "C" void me_engine_destroy(me_engine_t* engine) {
    delete engine;
}

extern "C" const char* me_engine_last_error(const me_engine_t* engine) {
    if (!engine) return "";
    /* mutex-locked read via const_cast: the handle is logically const but
     * the mutex protects internal state. */
    auto* mut = const_cast<me_engine*>(engine);
    std::lock_guard<std::mutex> lk(mut->error_mtx);
    return mut->last_error.c_str();
}
