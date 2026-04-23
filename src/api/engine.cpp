#include "media_engine/engine.h"
#include "core/engine_impl.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"

#include <new>

extern "C" me_status_t me_engine_create(const me_engine_config_t* config, me_engine_t** out) {
    if (!out) return ME_E_INVALID_ARG;
    auto* e = new (std::nothrow) me_engine{};
    if (!e) return ME_E_OUT_OF_MEMORY;
    if (config) e->config = *config;

    try {
        /* Resources owned by engine. Budget / codec-cache sizes will flow from
         * config once we define the keys; bootstrap uses defaults. */
        e->frames    = std::make_unique<me::resource::FramePool>(
                           e->config.memory_cache_bytes);
        e->codecs    = std::make_unique<me::resource::CodecPool>();
        e->scheduler = std::make_unique<me::sched::Scheduler>(
                           me::sched::Config{.cpu_threads = e->config.num_worker_threads},
                           *e->frames, *e->codecs);
    } catch (const std::exception& ex) {
        me::detail::set_error(e, ex.what());
        delete e;
        return ME_E_INTERNAL;
    }

    *out = e;
    return ME_OK;
}

extern "C" void me_engine_destroy(me_engine_t* engine) {
    if (!engine) return;
    /* Unique_ptr members tear down in reverse declaration order:
     * Scheduler → CodecPool → FramePool. Scheduler's dtor waits for all
     * outstanding work so no task sees a dangling pool. */
    delete engine;
}

extern "C" const char* me_engine_last_error(const me_engine_t* engine) {
    if (!engine) return "";
    auto* mut = const_cast<me_engine*>(engine);
    std::lock_guard<std::mutex> lk(mut->error_mtx);
    return mut->last_error.c_str();
}
