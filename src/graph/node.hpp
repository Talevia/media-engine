/*
 * Node — pure data. See docs/ARCHITECTURE_GRAPH.md §Graph 内部.
 *
 * A Node declares what to compute (kind), with what parameters (props),
 * reading which upstream outputs (inputs), producing which typed outputs
 * (outputs). It has no execute() method and no function pointers. The
 * kernel that implements this kind is looked up at dispatch time from
 * task::registry.
 */
#pragma once

#include "graph/types.hpp"

namespace me::task { enum class TaskKindId : uint32_t; }

namespace me::graph {

struct Node {
    task::TaskKindId        kind{};
    Properties              props;
    std::vector<InputPort>  inputs;
    std::vector<OutputPort> outputs;
    uint64_t                content_hash   = 0;
    bool                    time_invariant = false;
    /* Mirror of KindInfo::cacheable — copied at Builder::add time so the
     * scheduler can decide whether to peek/put without a registry lookup. */
    bool                    cacheable      = true;
};

}  // namespace me::graph
