/*
 * EvalInstance — per-invocation runtime state for one scheduler.evaluate_port
 * call against a Graph.
 *
 * Multiple EvalInstances can exist concurrently for the same Graph (e.g.
 * Exporter lookahead running frames N, N+1, N+2 simultaneously). They do not
 * interfere — each has its own output slots and cancel flag.
 *
 * See docs/ARCHITECTURE_GRAPH.md §Task 运行时.
 */
#pragma once

#include "graph/eval_context.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/types.h"

#include <atomic>
#include <string>
#include <vector>

namespace me::sched {

/* Per-EvalInstance node state — written during execution, read by scheduler. */
enum class NodeState : uint8_t {
    Idle      = 0,
    Running   = 1,
    Done      = 2,
    Failed    = 3,
};

class EvalInstance {
public:
    /* ctx is taken by value: scheduler patches the cache pointer into a
     * local copy before constructing the EvalInstance, and that copy needs
     * to outlive the original caller frame (kernels read ctx.cache during
     * task execution, which can outlast evaluate_port's return). */
    EvalInstance(const graph::Graph&, graph::EvalContext);

    const graph::Graph&        graph() const noexcept { return graph_; }
    const graph::EvalContext&  ctx()   const noexcept { return ctx_; }

    /* Access the per-node slot arrays. Indexed by NodeId.v then output port. */
    std::vector<graph::OutputSlot>& outputs_of(graph::NodeId);
    const graph::OutputSlot&        output_at(graph::PortRef) const;

    /* Inputs for a node are built just-in-time by the scheduler by copying
     * upstream outputs into this buffer before the kernel runs. */
    std::vector<graph::InputValue>& inputs_of(graph::NodeId);

    /* State transitions (simple setters — the scheduler owns the ordering). */
    NodeState state(graph::NodeId) const noexcept;
    void      set_state(graph::NodeId, NodeState) noexcept;

    void            set_error(me_status_t, std::string msg);
    me_status_t     error_status() const noexcept { return error_status_; }
    const std::string& error_message() const noexcept { return error_msg_; }

    std::atomic<bool>&       cancel_flag() noexcept { return cancel_; }
    const std::atomic<bool>& cancel_flag() const noexcept { return cancel_; }
    bool                     is_cancelled() const noexcept {
        return cancel_.load(std::memory_order_acquire);
    }

private:
    const graph::Graph&       graph_;
    graph::EvalContext        ctx_;
    std::vector<std::vector<graph::OutputSlot>> outputs_;
    std::vector<std::vector<graph::InputValue>> inputs_;
    std::vector<NodeState>    states_;

    std::atomic<bool> cancel_{false};
    me_status_t       error_status_ = ME_OK;
    std::string       error_msg_;
};

}  // namespace me::sched
