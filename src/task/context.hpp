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

namespace me::resource { class FramePool; class CodecPool; class GpuContext; }
namespace me::sched    { class OutputCache; }

namespace me::task {

struct TaskContext {
    me_rational_t            time{};          /* current EvalContext time */
    resource::FramePool*     frames = nullptr;
    resource::CodecPool*     codecs = nullptr;
    resource::GpuContext*    gpu    = nullptr; /* null if CPU kernel */
    const std::atomic<bool>* cancel = nullptr; /* per-EvalInstance cancel flag */
    sched::OutputCache*      cache  = nullptr; /* injected by scheduler; null when caller passes ctx without scheduler ownership */
};

}  // namespace me::task
