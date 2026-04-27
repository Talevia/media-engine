/*
 * Scheduler — drives Graph evaluation.
 *
 * Bootstrap capability: build a tf::Taskflow mirroring the Graph's topology
 * and run it through a tf::Executor. Every node → one tf::Task that reads
 * upstream outputs, invokes the registered kernel, writes outputs back into
 * the EvalInstance.
 *
 * Non-bootstrap (incoming later): heterogeneous pools (GPU / HW encoder /
 * I/O), cache peek-before-dispatch, lookahead pipelining. The bootstrap
 * pretends everything is Affinity::Cpu.
 *
 * See docs/ARCHITECTURE_GRAPH.md §Task 运行时 / §scheduler.
 */
#pragma once

#include "graph/eval_context.hpp"
#include "graph/eval_error.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "media_engine/types.h"
#include "scheduler/eval_instance.hpp"
#include "scheduler/output_cache.hpp"

#include <taskflow/taskflow.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace me::resource {
    class FramePool;
    class CodecPool;
    template <typename T> class StatefulResourcePool;
}
namespace me::audio { class TempoStretcher; }

namespace me::sched {

struct Config {
    int  cpu_threads     = 0;      /* 0 = hardware_concurrency */
    /* OutputCache capacity (entries). 0 disables caching; default sized
     * for typical scrubbing workloads. Per-frame RGBA at 4K
     * (~33 MB) × 256 ≈ 8 GB worst case; common 720p (~3.7 MB) × 256
     * ≈ 950 MB. Tune down via Engine config when memory is tight. */
    std::size_t output_cache_capacity = 256;
    /* GPU / HW / IO pools arrive with their respective backlog items. */
};

class Scheduler {
public:
    Scheduler(const Config&, resource::FramePool&, resource::CodecPool&);
    ~Scheduler();

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /* Submit a graph evaluation targeting `terminal`. Returns immediately.
     * Work runs on the CPU pool; caller awaits via Future<T>::await().
     *
     * The returned Future's T must match the variant arm of the terminal
     * output type; mismatch raises at await() time. */
    template<typename T>
    graph::Future<T> evaluate_port(const graph::Graph&       g,
                                   graph::PortRef            terminal,
                                   const graph::EvalContext& ctx);

    /* Convenience: build + await in one call. */
    template<typename T>
    me_status_t wait(graph::Future<T>& f, T* out, std::string* err);

    /* Diagnostic access to the output cache (hit/miss counters, size).
     * Mutable in case future config wants runtime resizing. */
    OutputCache&       cache()       noexcept { return cache_; }
    const OutputCache& cache() const noexcept { return cache_; }

    /* Inject a stateful TempoStretcher pool. Engine constructs the
     * pool when ME_WITH_SOUNDTOUCH is on and hands the pointer here;
     * run_node() flows it through TaskContext.tempo_pool. Null when
     * disabled — AudioTimestretch kernel falls back to fresh-per-call
     * (state continuity lost). */
    void set_tempo_pool(resource::StatefulResourcePool<audio::TempoStretcher>* p) {
        tempo_pool_ = p;
    }
    resource::StatefulResourcePool<audio::TempoStretcher>* tempo_pool() const noexcept {
        return tempo_pool_;
    }

private:
    /* Internal build: returns (run_future, eval_instance) pair. Separated from
     * the template so the bulk of the logic stays out of the header. */
    std::pair<std::shared_future<void>, std::shared_ptr<EvalInstance>>
        build_and_run(const graph::Graph&, const graph::EvalContext&);

    resource::FramePool&                                        frames_;
    resource::CodecPool&                                        codecs_;
    resource::StatefulResourcePool<audio::TempoStretcher>*      tempo_pool_ = nullptr;
    OutputCache                                                 cache_;
    tf::Executor                                                cpu_;
};

/* ---- template impl ------------------------------------------------------ */

template<typename T>
graph::Future<T> Scheduler::evaluate_port(const graph::Graph&       g,
                                          graph::PortRef            terminal,
                                          const graph::EvalContext& ctx) {
    auto [run_fut, eval] = build_and_run(g, ctx);
    return graph::Future<T>(std::move(run_fut), std::move(eval), terminal);
}

template<typename T>
me_status_t Scheduler::wait(graph::Future<T>& f, T* out, std::string* err) {
    try {
        T v = f.await();
        if (out) *out = std::move(v);
        return ME_OK;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return ME_E_INTERNAL;
    }
}

}  // namespace me::sched

/* ---- graph::Future<T>::await — depends on sched::EvalInstance ---------- */

namespace me::graph {

template<typename T>
T Future<T>::await() {
    if (!eval_ || !run_future_.valid()) {
        throw EvalError(ME_E_INTERNAL, "Future::await on empty future");
    }
    run_future_.wait();
    if (eval_->error_status() != ME_OK) {
        /* Surface the kernel's status code to the caller — previously
         * this was a plain runtime_error so api/* layers couldn't
         * distinguish ME_E_IO from ME_E_DECODE. Callers that only
         * `catch (const std::exception&)` still match because
         * EvalError publicly inherits runtime_error. */
        throw EvalError(eval_->error_status(),
                         eval_->error_message().empty() ? "kernel failed"
                                                          : eval_->error_message());
    }
    const auto& slot = eval_->output_at(terminal_);
    if (!std::holds_alternative<T>(slot.v)) {
        throw EvalError(ME_E_INTERNAL, "Future::await: terminal output type mismatch");
    }
    return std::get<T>(slot.v);
}

template<typename T>
void Future<T>::cancel() {
    if (eval_) eval_->cancel_flag().store(true, std::memory_order_release);
}

}  // namespace me::graph
