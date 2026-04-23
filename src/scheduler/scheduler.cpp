#include "scheduler/scheduler.hpp"

#include "graph/eval_context.hpp"
#include "resource/frame_pool.hpp"
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

/* Run one node's kernel. Looks up kernel, gathers inputs, constructs
 * TaskContext, invokes, writes outputs. On failure, records error + sets
 * cancel flag so downstream nodes short-circuit. */
void run_node(graph::NodeId       id,
              EvalInstance&       eval,
              resource::FramePool& frames,
              resource::CodecPool& codecs) {
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
    ctx.time   = eval.ctx().time;
    ctx.frames = &frames;
    ctx.codecs = &codecs;
    ctx.gpu    = eval.ctx().gpu;
    ctx.cancel = &eval.cancel_flag();
    ctx.cache  = eval.ctx().cache;

    eval.set_state(id, NodeState::Running);

    auto& outs = eval.outputs_of(id);
    me_status_t status = ME_OK;
    try {
        status = kernel(ctx, node.props,
                        std::span<const graph::InputValue>{ins.data(), ins.size()},
                        std::span<graph::OutputSlot>      {outs.data(), outs.size()});
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

    eval.set_state(id, NodeState::Done);
}

}  // namespace

Scheduler::Scheduler(const Config& cfg,
                     resource::FramePool& frames,
                     resource::CodecPool& codecs)
    : frames_(frames),
      codecs_(codecs),
      cpu_(cfg.cpu_threads > 0
               ? static_cast<size_t>(cfg.cpu_threads)
               : std::thread::hardware_concurrency()) {}

Scheduler::~Scheduler() {
    cpu_.wait_for_all();
}

std::pair<std::shared_future<void>, std::shared_ptr<EvalInstance>>
Scheduler::build_and_run(const graph::Graph& g, const graph::EvalContext& ctx) {
    auto eval = std::make_shared<EvalInstance>(g, ctx);
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
            run_node(id, *eval_raw, frames_, codecs_);
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
