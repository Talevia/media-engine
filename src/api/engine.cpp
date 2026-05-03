#include "media_engine/engine.h"
#include "compose/affine_blit_kernel.hpp"
#include "compose/compose_cpu_kernel.hpp"
#include "compose/convert_rgba8_kernel.hpp"
#include "compose/cross_dissolve_kernel.hpp"
#include "audio/mix_kernel.hpp"
#include "audio/resample_kernel.hpp"
#ifdef ME_HAS_SOUNDTOUCH
#include "audio/tempo.hpp"
#include "audio/timestretch_kernel.hpp"
#include "resource/stateful_pool.hpp"
#endif
#include "compose/encode_png_kernel.hpp"
#include "compose/body_alpha_key_stage.hpp"
#include "compose/chromatic_aberration_stage.hpp"
#include "compose/displacement_stage.hpp"
#include "compose/face_mosaic_stage.hpp"
#include "compose/face_sticker_stage.hpp"
#include "compose/film_grain_stage.hpp"
#include "compose/glitch_stage.hpp"
#include "compose/hue_saturation_stage.hpp"
#include "compose/motion_blur_stage.hpp"
#include "compose/ordered_dither_stage.hpp"
#include "compose/posterize_stage.hpp"
#include "compose/radial_blur_stage.hpp"
#include "compose/scan_lines_stage.hpp"
#include "compose/tilt_shift_stage.hpp"
#include "compose/tone_curve_stage.hpp"
#include "compose/vignette_stage.hpp"
#include "compose/warp_stage.hpp"
#include "core/engine_impl.hpp"
#include "io/decode_audio_kernel.hpp"
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
        me::io::register_decode_audio_kind();
        me::compose::register_convert_rgba8_kind();
        me::compose::register_affine_blit_kind();
        me::compose::register_compose_cpu_kind();
        me::compose::register_cross_dissolve_kind();
        me::compose::register_encode_png_kind();
        me::compose::register_face_sticker_kind();
        me::compose::register_face_mosaic_kind();
        me::compose::register_body_alpha_key_kind();
        me::compose::register_tone_curve_kind();
        me::compose::register_hue_saturation_kind();
        me::compose::register_vignette_kind();
        me::compose::register_film_grain_kind();
        me::compose::register_glitch_kind();
        me::compose::register_scan_lines_kind();
        me::compose::register_chromatic_aberration_kind();
        me::compose::register_posterize_kind();
        me::compose::register_ordered_dither_kind();
        me::compose::register_motion_blur_kind();
        me::compose::register_radial_blur_kind();
        me::compose::register_tilt_shift_kind();
        me::compose::register_warp_kind();
        me::compose::register_displacement_kind();
        me::audio::register_resample_kind();
        me::audio::register_mix_kind();
#ifdef ME_HAS_SOUNDTOUCH
        me::audio::register_timestretch_kind();
#endif
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
#ifdef ME_HAS_SOUNDTOUCH
        /* Pool factory left empty — AudioTimestretch kernel is
         * the constructor since it knows the per-input rate +
         * channels (StatefulResourcePool::adopt path). */
        e->tempo_pool   = std::make_unique<
            me::resource::StatefulResourcePool<me::audio::TempoStretcher>>(
                []() { return std::unique_ptr<me::audio::TempoStretcher>(); });
#endif
        e->scheduler    = std::make_unique<me::sched::Scheduler>(
                              me::sched::Config{.cpu_threads = e->config.num_worker_threads},
                              *e->frames, *e->codecs);
#ifdef ME_HAS_SOUNDTOUCH
        e->scheduler->set_tempo_pool(e->tempo_pool.get());
#endif
#ifdef ME_HAS_INFERENCE
        /* Process-wide inference asset cache. Capacity 64 entries
         * = roughly two seconds of 30fps detection-driven scrubbing
         * before LRU evicts; small enough to bound memory at typical
         * Tensor footprints, large enough to matter for the
         * scrubbing / re-export workloads M11 §137 calls out.
         * Tuning belongs in the cache benchmark when it lands;
         * config knob is deferred until then. */
        e->asset_cache = std::make_unique<me::inference::AssetCache>(64);
#endif
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
