/*
 * Graph + Graph::Builder.
 *
 * Graph is an immutable DAG of Nodes produced by Builder::build(). It has
 * no runtime state — all per-evaluation state lives in sched::EvalInstance.
 *
 * See docs/ARCHITECTURE_GRAPH.md §Graph 内部.
 */
#pragma once

#include "graph/node.hpp"
#include "graph/types.hpp"
#include "task/task_kind.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace me::graph {

class Graph {
public:
    /* Read-only queries after build(). */
    std::span<const Node>    nodes() const noexcept { return nodes_; }
    std::span<const NodeId>  topo_order() const noexcept { return topo_; }
    std::span<const NodeId>  dependents(NodeId n) const noexcept {
        return n.v < dependents_.size()
            ? std::span<const NodeId>{dependents_[n.v]}
            : std::span<const NodeId>{};
    }
    uint64_t                 content_hash() const noexcept { return content_hash_; }

    /* Named terminals (e.g., "video" / "audio"). */
    std::vector<std::string_view> terminal_names() const;
    std::optional<PortRef>        terminal(std::string_view name) const;
    void                          set_terminal(std::string name, PortRef port);

    class Builder {
    public:
        /* Add a Node. inputs are upstream PortRefs (from prior add()s). Returns
         * the new NodeId. The TaskKindId must be registered in task::registry
         * before build() — build() uses the schema to size output slots and
         * populate content hashes. */
        NodeId add(task::TaskKindId,
                   Properties,
                   std::span<const PortRef> inputs);

        /* Convenience overload: accept a brace-list of PortRefs. */
        NodeId add(task::TaskKindId kind,
                   Properties       props,
                   std::initializer_list<PortRef> inputs) {
            return add(kind, std::move(props),
                       std::span<const PortRef>{inputs.begin(), inputs.size()});
        }

        /* Name a terminal output for orchestrators to request via Graph::terminal. */
        void name_terminal(std::string name, PortRef);

        /* Finalize: run topo sort, compute content_hash per node, freeze. */
        Graph build() &&;

    private:
        std::vector<Node>                             draft_nodes_;
        std::vector<std::pair<std::string, PortRef>>  named_terminals_;
    };

private:
    std::vector<Node>                       nodes_;
    std::vector<NodeId>                     topo_;
    std::vector<std::vector<NodeId>>        dependents_;
    std::unordered_map<std::string, PortRef> terminals_;
    uint64_t                                content_hash_ = 0;
};

}  // namespace me::graph
