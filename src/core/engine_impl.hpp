/* Internal — not exported.
 *
 * ME_HAS_X visibility constraint
 * ------------------------------
 * Every `ME_HAS_X` macro this header consults to gate a struct field
 * (today: ME_HAS_SOUNDTOUCH for `tempo_pool`, ME_HAS_INFERENCE for
 * the model-fetcher members) MUST be defined `PUBLIC` or `INTERFACE`
 * on the `media_engine` target in `src/CMakeLists.txt`. PRIVATE
 * silently diverges struct layout between libmedia_engine.a and any
 * test TU that includes this header — cycle 41 burned 7 cycles
 * diagnosing the resulting `pthread_mutex_lock EINVAL` flake. The
 * `engine_impl_compile_defs_audit` ctest enforces the rule via
 * `tools/check-engine-impl-compile-defs.sh`; raising a guarded
 * macro to PRIVATE breaks the test loop with an explicit
 * remediation message. */
#pragma once

#include "media_engine/engine.h"
#include "gpu/gpu_backend.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/codec_pool.hpp"
#include "resource/disk_cache.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"

#ifdef ME_HAS_SOUNDTOUCH
#include "audio/tempo.hpp"
#include "resource/stateful_pool.hpp"
#endif

#ifdef ME_HAS_INFERENCE
#include "inference/asset_cache.hpp"
#include "inference/model_loader.hpp"
#include "media_engine/ml.h"
#endif

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>

struct me_engine {
    me_engine_config_t config{};

    /* Resources owned by engine lifetime. Declaration order matters —
     * Scheduler holds references to FramePool and CodecPool, so those must
     * outlive it. Destruction is reverse order (Scheduler first, good).
     *
     * gpu_backend is always populated (NullGpuBackend default; BgfxGpuBackend
     * under ME_WITH_GPU=ON). Placed before Scheduler so it outlives any
     * kernel that might consult `available()` during task execution; lands
     * independent of frame/codec pools. */
    std::unique_ptr<me::gpu::GpuBackend>           gpu_backend;
    std::unique_ptr<me::resource::FramePool>       frames;
    std::unique_ptr<me::resource::CodecPool>       codecs;
    std::unique_ptr<me::resource::AssetHashCache>  asset_hashes;
    /* DiskCache populated iff me_engine_config_t.cache_dir is non-
     * null + non-empty. Disabled instance when absent (put/get
     * silently no-op). Consumer is `me_render_frame`'s scrubbing
     * cache-aware path (asset_hash:source_t key, src/api/render.cpp). */
    std::unique_ptr<me::resource::DiskCache>       disk_cache;
#ifdef ME_HAS_SOUNDTOUCH
    /* Per-track SoundTouch instance pool. AudioTimestretch kernel
     * borrows by instance_key (orchestrator-supplied stable id),
     * preserving SoundTouch's internal pitch buffer across chunked
     * invocations. Lives before scheduler so it outlives any task
     * that holds a Handle. */
    std::unique_ptr<me::resource::StatefulResourcePool<me::audio::TempoStretcher>>
        tempo_pool;
#endif
    std::unique_ptr<me::sched::Scheduler>          scheduler;

#ifdef ME_HAS_INFERENCE
    /* Host-supplied model fetcher (M11 ml-model-lazy-load-callback).
     * Stored verbatim by `me_engine_set_model_fetcher`; consulted by
     * the inference runtime when an ML effect references a model
     * via {model_id, model_version, quantization}. Pre-runtime
     * cycles, the storage is exercised but the callback is never
     * invoked. Mutex protects races between host registration and
     * runtime fetch (both can happen from any thread). */
    std::mutex          model_fetcher_mu;
    me_model_fetcher_t  model_fetcher_cb   = nullptr;
    void*               model_fetcher_user = nullptr;

    /* Engine-level cache of validated model blobs (M11
     * inference-load-model-blob-wire-effect-stages). Keyed on
     * (model_id, version, quantization); value is the
     * `me::inference::LoadedModel` returned by
     * `load_model_blob` after license-whitelist + content_hash
     * validation. `load_model_blob` consults this cache on entry
     * and stores after successful validation, so subsequent
     * loads of the same model identity skip the host fetcher
     * round-trip. Cleared via
     * `me::inference::clear_loaded_models` (test reset path). */
    std::mutex loaded_models_mu;
    std::map<std::tuple<std::string, std::string, std::string>,
             me::inference::LoadedModel> loaded_models;

    /* Process-wide AssetCache shared across all effect kernels
     * (M11 inference-asset-cache-wire-effect-stages, M11 exit
     * criterion at docs/MILESTONES.md:137). Effect kernels never
     * call `me::inference::Runtime::run` directly — they go
     * through `me::inference::run_cached(engine, runtime, ...)`
     * which consults this cache before delegating + stores on
     * miss. Capacity comes from a fixed default at engine_create
     * time; future config knob can override if profiling motivates
     * it. */
    std::unique_ptr<me::inference::AssetCache> asset_cache;
#endif
};

namespace me::detail {

/* Thread-local last-error storage, keyed by engine pointer.
 *
 * Contract (docs/API.md): thread A's errors do not clobber thread B's view.
 * Each thread sees only the last error it itself produced on a given engine.
 *
 * The map is function-scope thread_local inside an inline function so the
 * one-per-thread instance is merged across TUs. Lookup / insert is O(1)
 * average on unordered_map — no locks needed.
 *
 * Async paths (Exporter worker): the worker runs on a different thread from
 * the API caller and must NOT populate last-error directly — the caller's
 * thread-local slot is a different map entirely. Instead, the worker stashes
 * the message on its Job struct; me_render_wait copies it into the caller's
 * thread-local slot after join (see src/api/render.cpp). */
inline std::unordered_map<const me_engine*, std::string>& thread_errors() {
    thread_local std::unordered_map<const me_engine*, std::string> map;
    return map;
}

inline void set_error(const me_engine* eng, std::string msg) {
    if (!eng) return;
    thread_errors()[eng] = std::move(msg);
}

inline void clear_error(const me_engine* eng) {
    if (!eng) return;
    thread_errors().erase(eng);
}

inline const char* get_error(const me_engine* eng) {
    if (!eng) return "";
    auto& m = thread_errors();
    auto it = m.find(eng);
    return it != m.end() ? it->second.c_str() : "";
}

}  // namespace me::detail
