/* Internal — not exported. */
#pragma once

#include "media_engine/engine.h"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"

#include <memory>
#include <mutex>
#include <string>

struct me_engine {
    me_engine_config_t config{};

    /* Resources owned by engine lifetime. Declaration order matters —
     * Scheduler holds references to FramePool and CodecPool, so those must
     * outlive it. Destruction is reverse order (Scheduler first, good). */
    std::unique_ptr<me::resource::FramePool>  frames;
    std::unique_ptr<me::resource::CodecPool>  codecs;
    std::unique_ptr<me::sched::Scheduler>     scheduler;

    /* NOTE: API.md specifies thread-local last-error per engine.
     * Phase-1 stub uses a single string under a mutex. Upgrade to
     * a thread_local slot once worker threads use it (backlog item
     * debt-thread-local-last-error). */
    std::mutex  error_mtx;
    std::string last_error;
};

namespace me::detail {

inline void set_error(me_engine* eng, std::string msg) {
    if (!eng) return;
    std::lock_guard<std::mutex> lk(eng->error_mtx);
    eng->last_error = std::move(msg);
}

inline void clear_error(me_engine* eng) {
    if (!eng) return;
    std::lock_guard<std::mutex> lk(eng->error_mtx);
    eng->last_error.clear();
}

}  // namespace me::detail
