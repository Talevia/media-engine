/* Internal — not exported. */
#pragma once

#include "media_engine/engine.h"
#include "gpu/gpu_backend.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/codec_pool.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"

#include <memory>
#include <string>
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
    std::unique_ptr<me::sched::Scheduler>          scheduler;
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
