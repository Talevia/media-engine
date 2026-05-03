#include "scheduler/scheduler.hpp"

#include "graph/eval_context.hpp"
#include "graph/eval_error.hpp"
#include "resource/frame_pool.hpp"
#include "resource/stateful_pool.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace me::sched {

namespace {

/* Cache key per the contract in ARCHITECTURE_GRAPH §缓存集成:
 *   key = node.time_invariant
 *       ? node.content_hash
 *       : hash_combine(node.content_hash, eval_ctx.time)
 *
 * IoDecodeVideo et al. carry source_t in props (not ctx.time), so their
 * content_hash already varies with time and the time mix is redundant
 * but harmless. Time-invariant kernels (RenderConvertRgba8) skip the
 * mix and hit identically across frames consuming the same upstream
 * value. */
inline uint64_t compute_cache_key(const graph::Node& node,
                                   const graph::EvalContext& ctx) {
    return node.time_invariant
        ? node.content_hash
        : mix_time_into_hash(node.content_hash, ctx.time);
}

/* Run one node's kernel. Looks up kernel, gathers inputs, constructs
 * TaskContext, invokes, writes outputs. On failure, records error + sets
 * cancel flag so downstream nodes short-circuit. */
void run_node(graph::NodeId       id,
              EvalInstance&       eval,
              resource::FramePool& frames,
              resource::CodecPool& codecs,
              resource::StatefulResourcePool<audio::TempoStretcher>* tempo_pool) {
    if (eval.is_cancelled()) {
        eval.set_state(id, NodeState::Failed);
        return;
    }

    const auto& g    = eval.graph();
    const auto& node = g.nodes()[id.v];

    /* Gather inputs by copying from upstream outputs. */
    auto& ins = eval.inputs_of(id);
    for (size_t i = 0; i < node.inputs.size(); ++i) {
        ins[i] = eval.output_at(node.inputs[i].source);
    }

    auto& outs = eval.outputs_of(id);

    /* Cache peek-before-dispatch. Only fires when the scheduler injected
     * a cache pointer (always for engine-owned schedulers), the kernel
     * declared itself cacheable (KindInfo::cacheable; false for kernels
     * that emit stateful handles like IoDemux's AVFormatContext), AND
     * every output port has a hit. Partial hits fall through to the
     * kernel — caching individual ports independently would require
     * splitting the kernel call too, which the per-kernel ABI doesn't
     * support. */
    OutputCache* cache = node.cacheable ? eval.ctx().cache : nullptr;
    if (cache && !outs.empty()) {
        const uint64_t key = compute_cache_key(node, eval.ctx());
        bool all_hit = true;
        std::vector<graph::OutputSlot> peeked(outs.size());
        for (size_t p = 0; p < outs.size(); ++p) {
            auto v = cache->get(key, static_cast<uint8_t>(p));
            if (!v) { all_hit = false; break; }
            peeked[p].v = std::move(*v);
        }
        if (all_hit) {
            for (size_t p = 0; p < outs.size(); ++p) {
                outs[p] = std::move(peeked[p]);
            }
            eval.set_state(id, NodeState::Done);
            return;
        }
    }

    /* Look up kernel. */
    task::KernelFn kernel = task::best_kernel_for(node.kind, task::Affinity::Cpu);
    if (!kernel) {
        eval.set_error(ME_E_UNSUPPORTED,
            "no kernel registered for TaskKindId " +
            std::to_string(static_cast<uint32_t>(node.kind)));
        eval.set_state(id, NodeState::Failed);
        eval.cancel_flag().store(true, std::memory_order_release);
        return;
    }

    task::TaskContext ctx;
    ctx.time       = eval.ctx().time;
    ctx.frames     = &frames;
    ctx.codecs     = &codecs;
    ctx.gpu        = eval.ctx().gpu;
    ctx.cancel     = &eval.cancel_flag();
    ctx.cache      = eval.ctx().cache;
    ctx.engine     = eval.ctx().engine;
    ctx.tempo_pool = tempo_pool;

    eval.set_state(id, NodeState::Running);

    me_status_t status = ME_OK;
    try {
        status = kernel(ctx, node.props,
                        std::span<const graph::InputValue>{ins.data(), ins.size()},
                        std::span<graph::OutputSlot>      {outs.data(), outs.size()});
    } catch (const graph::EvalError& e) {
        /* Kernel raised a typed status — preserve both the status
         * and message so the api-layer caller can surface the
         * original code (e.g. ME_E_IO from avformat_open_input
         * failure rather than the generic ME_E_INTERNAL). */
        eval.set_error(e.status(), e.what());
        eval.set_state(id, NodeState::Failed);
        eval.cancel_flag().store(true, std::memory_order_release);
        return;
    } catch (const std::exception& e) {
        eval.set_error(ME_E_INTERNAL, e.what());
        eval.set_state(id, NodeState::Failed);
        eval.cancel_flag().store(true, std::memory_order_release);
        return;
    }

    if (status != ME_OK) {
        eval.set_error(status, "kernel returned non-OK status");
        eval.set_state(id, NodeState::Failed);
        eval.cancel_flag().store(true, std::memory_order_release);
        return;
    }

    /* Cache fill on success. Same key as the peek above. shared_ptr arms
     * inside the variant amortize the cost of "store a frame" to a
     * refcount bump. */
    if (cache && !outs.empty()) {
        const uint64_t key = compute_cache_key(node, eval.ctx());
        for (size_t p = 0; p < outs.size(); ++p) {
            cache->put(key, static_cast<uint8_t>(p), outs[p].v);
        }
    }

    eval.set_state(id, NodeState::Done);
}

}  // namespace

Scheduler::Scheduler(const Config& cfg,
                     resource::FramePool& frames,
                     resource::CodecPool& codecs)
    : frames_(frames),
      codecs_(codecs),
      cache_(cfg.output_cache_capacity),
      cpu_(cfg.cpu_threads > 0
               ? static_cast<size_t>(cfg.cpu_threads)
               : std::thread::hardware_concurrency()) {}

Scheduler::~Scheduler() {
    cpu_.wait_for_all();
}

std::pair<std::shared_future<void>, std::shared_ptr<EvalInstance>>
Scheduler::build_and_run(const graph::Graph& g, const graph::EvalContext& ctx) {
    /* Patch the EvalContext's cache pointer to our owned OutputCache before
     * the EvalInstance copies it. Caller can override by populating
     * ctx.cache themselves with another OutputCache instance — useful for
     * test scaffolds that want to observe cache behavior in isolation. */
    graph::EvalContext patched = ctx;
    if (!patched.cache) patched.cache = &cache_;
    auto eval = std::make_shared<EvalInstance>(g, patched);
    /* Taskflow object must outlive the executor.run call. Store it alongside
     * the shared_future via a small holder owned by the shared_future's
     * continuation. Simplest: keep the flow in a shared_ptr and capture it
     * in an async wrapper. */
    auto flow = std::make_shared<tf::Taskflow>();

    const auto nodes = g.nodes();
    std::vector<tf::Task> tf_tasks;
    tf_tasks.reserve(nodes.size());

    for (size_t i = 0; i < nodes.size(); ++i) {
        const graph::NodeId id{static_cast<uint32_t>(i)};
        tf_tasks.push_back(flow->emplace([id, eval_raw = eval.get(), this]() {
            run_node(id, *eval_raw, frames_, codecs_, tempo_pool_);
        }));
    }

    /* Wire dependencies: for each node, each input's source precedes this node. */
    for (size_t i = 0; i < nodes.size(); ++i) {
        for (const auto& in : nodes[i].inputs) {
            tf_tasks[in.source.node.v].precede(tf_tasks[i]);
        }
    }

    auto fut = cpu_.run(*flow);

    /* Hand both the tf::Taskflow and the tf::Future into a std::shared_future
     * so they live until all awaiters are done. */
    auto holder = std::async(std::launch::async,
        [flow, fut = std::move(fut)]() mutable {
            fut.wait();
        });
    return {holder.share(), std::move(eval)};
}

}  // namespace me::sched
