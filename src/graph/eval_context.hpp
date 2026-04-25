/*
 * EvalContext — the inputs to one scheduler.evaluate_port call.
 *
 * Lives with graph/ because it's part of the graph-evaluation contract, not
 * scheduler internals. Kernels receive this (via TaskContext) at dispatch.
 *
 * See docs/ARCHITECTURE_GRAPH.md §用户面 API / §Task 运行时.
 */
#pragma once

#include "media_engine/types.h"

namespace me::resource { class FramePool; class CodecPool; class GpuContext; }
namespace me::sched    { class OutputCache; }

namespace me::graph {

struct EvalContext {
    me_rational_t            time{0, 1};   /* time_invariant nodes ignore this */
    resource::FramePool*     frames = nullptr;
    resource::CodecPool*     codecs = nullptr;
    resource::GpuContext*    gpu    = nullptr;
    /* Scheduler injects its OutputCache here at build_and_run time; callers
     * passing an EvalContext don't need to set this (left null = no cache). */
    sched::OutputCache*      cache  = nullptr;
};

}  // namespace me::graph
