/*
 * TaskContext — the "world" injected into every kernel at dispatch time.
 *
 * Kernels are pure function pointers; everything they need (resources,
 * time, cancel token, cache) comes through TaskContext. See
 * docs/ARCHITECTURE_GRAPH.md §Task 运行时.
 */
#pragma once

#include "media_engine/types.h"

#include <atomic>

struct me_engine;

namespace me::resource {
    class FramePool;
    class CodecPool;
    class GpuContext;
    template <typename T> class StatefulResourcePool;
}
namespace me::audio { class TempoStretcher; }
namespace me::sched { class OutputCache; }

namespace me::task {

struct TaskContext {
    me_rational_t            time{};          /* current EvalContext time */
    resource::FramePool*     frames = nullptr;
    resource::CodecPool*     codecs = nullptr;
    resource::GpuContext*    gpu    = nullptr; /* null if CPU kernel */
    const std::atomic<bool>* cancel = nullptr; /* per-EvalInstance cancel flag */
    sched::OutputCache*      cache  = nullptr; /* injected by scheduler; null when caller passes ctx without scheduler ownership */
    /* Optional engine pointer threaded from EvalContext.engine.
     * Kernels that dispatch into runtime-mode helpers (e.g. the
     * ML inference resolvers in src/compose/landmark_resolver
     * + mask_resolver) read this. Null in test contexts that
     * don't need it. */
    ::me_engine*             engine = nullptr;

    /* Stateful resource pool for SoundTouch instances. Kernels (e.g.
     * AudioTimestretch) borrow per-track instances keyed by an
     * instance_key prop, preserving SoundTouch's internal pitch
     * buffer state across chunked invocations within a Player
     * session. Null-allowed: kernels fall back to a fresh-per-call
     * instance when missing (state continuity lost). */
    resource::StatefulResourcePool<audio::TempoStretcher>* tempo_pool = nullptr;
};

}  // namespace me::task
