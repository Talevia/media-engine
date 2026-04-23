#include "scheduler/eval_instance.hpp"
#include "graph/eval_context.hpp"

#include <stdexcept>
#include <utility>

namespace me::sched {

EvalInstance::EvalInstance(const graph::Graph& g, const graph::EvalContext& c)
    : graph_(g), ctx_(c),
      outputs_(g.nodes().size()),
      inputs_(g.nodes().size()),
      states_(g.nodes().size(), NodeState::Idle) {
    auto nodes = g.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        outputs_[i].resize(nodes[i].outputs.size());
        inputs_[i].resize(nodes[i].inputs.size());
    }
}

std::vector<graph::OutputSlot>& EvalInstance::outputs_of(graph::NodeId id) {
    return outputs_.at(id.v);
}

const graph::OutputSlot& EvalInstance::output_at(graph::PortRef p) const {
    return outputs_.at(p.node.v).at(p.port_idx);
}

std::vector<graph::InputValue>& EvalInstance::inputs_of(graph::NodeId id) {
    return inputs_.at(id.v);
}

NodeState EvalInstance::state(graph::NodeId id) const noexcept {
    return states_[id.v];
}

void EvalInstance::set_state(graph::NodeId id, NodeState s) noexcept {
    states_[id.v] = s;
}

void EvalInstance::set_error(me_status_t s, std::string msg) {
    if (error_status_ == ME_OK) {   /* keep the first reported error */
        error_status_ = s;
        error_msg_    = std::move(msg);
    }
}

}  // namespace me::sched
