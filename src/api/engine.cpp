#include "media_engine/engine.h"
#include "compose/affine_blit_kernel.hpp"
#include "compose/convert_rgba8_kernel.hpp"
#include "core/engine_impl.hpp"
#include "io/decode_video_kernel.hpp"
#include "io/demux_kernel.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"

#include <mutex>
#include <new>
/* `std::call_once` lives in <mutex>; me_engine itself no longer holds a
 * mutex now that last-error is thread_local per engine. */

extern "C" me_status_t me_engine_create(const me_engine_config_t* config, me_engine_t** out) {
    if (!out) return ME_E_INVALID_ARG;

    /* Register built-in task kinds once per process. register_kind is
     * idempotent (re-registering the same kind overwrites with the same
     * info), but std::call_once avoids redundant work + is the conventional
     * static-init pattern in C++. */
    static std::once_flag kinds_once;
    std::call_once(kinds_once, []() {
        me::io::register_demux_kind();
        me::io::register_decode_video_kind();
        me::compose::register_convert_rgba8_kind();
        me::compose::register_affine_blit_kind();
    });

    auto* e = new (std::nothrow) me_engine{};
    if (!e) return ME_E_OUT_OF_MEMORY;
    if (config) e->config = *config;

    try {
        /* Resources owned by engine. Budget / codec-cache sizes will flow from
         * config once we define the keys; bootstrap uses defaults. */
        e->gpu_backend  = me::gpu::make_gpu_backend();
        e->frames       = std::make_unique<me::resource::FramePool>(
                              e->config.memory_cache_bytes);
        e->codecs       = std::make_unique<me::resource::CodecPool>();
        e->asset_hashes = std::make_unique<me::resource::AssetHashCache>();
        /* DiskCache takes cache_dir from config (empty string =
         * disabled). Size cap comes from disk_cache_bytes (0 =
         * unlimited). Ctor scans the existing directory (if any)
         * to seed the on-disk-bytes running total but doesn't
         * create it — first put() does the mkdir lazily. */
        e->disk_cache   = std::make_unique<me::resource::DiskCache>(
                              e->config.cache_dir ? std::string(e->config.cache_dir)
                                                  : std::string{},
                              e->config.disk_cache_bytes);
        e->scheduler    = std::make_unique<me::sched::Scheduler>(
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
    return me::detail::get_error(engine);
}
