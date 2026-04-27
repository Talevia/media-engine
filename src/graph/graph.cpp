#include "graph/graph.hpp"
#include "task/registry.hpp"

#include <algorithm>
#include <cstring>
#include <queue>
#include <stdexcept>

namespace me::graph {

namespace {

/* FNV-1a 64-bit — tiny, deterministic, sufficient for content-addressing.
 * Can be swapped to xxhash later without API change. */
constexpr uint64_t kFnv64Offset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnv64Prime  = 0x00000100000001b3ULL;

inline uint64_t fnv_mix(uint64_t h, const void* data, size_t len) {
    auto p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= kFnv64Prime;
    }
    return h;
}

inline uint64_t fnv_mix_u64(uint64_t h, uint64_t v) {
    return fnv_mix(h, &v, sizeof(v));
}

uint64_t hash_input_value(uint64_t h, const InputValue& v) {
    h = fnv_mix_u64(h, static_cast<uint64_t>(v.type()));
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            /* nothing to mix */
        } else if constexpr (std::is_same_v<T, int64_t>) {
            h = fnv_mix_u64(h, static_cast<uint64_t>(x));
        } else if constexpr (std::is_same_v<T, double>) {
            uint64_t bits = 0;
            std::memcpy(&bits, &x, sizeof(bits));
            h = fnv_mix_u64(h, bits);
        } else if constexpr (std::is_same_v<T, bool>) {
            h = fnv_mix_u64(h, x ? 1ULL : 0ULL);
        } else if constexpr (std::is_same_v<T, std::string>) {
            h = fnv_mix(h, x.data(), x.size());
        } else {
            /* shared_ptr<FrameHandle> / shared_ptr<DemuxContext>: identity is
             * intentionally NOT part of the content hash — these are stateful
             * runtime artifacts. Properties of these types should not be used
             * as cache keys (and in practice kernels receive them via input
             * ports, not properties). */
            h = fnv_mix_u64(h, 0xFFULL);
        }
    }, v.v);
    return h;
}

uint64_t hash_node_static(const Node& n,
                          std::span<const uint64_t> input_hashes) {
    uint64_t h = kFnv64Offset;
    h = fnv_mix_u64(h, static_cast<uint64_t>(n.kind));
    /* Properties are ordered (std::map), so iteration order is deterministic. */
    for (const auto& [k, v] : n.props) {
        h = fnv_mix(h, k.data(), k.size());
        h = hash_input_value(h, v);
    }
    for (uint64_t ih : input_hashes) h = fnv_mix_u64(h, ih);
    return h;
}

}  // namespace

NodeId Graph::Builder::add(task::TaskKindId kind,
                           Properties props,
                           std::span<const PortRef> input_refs) {
    Node n;
    n.kind  = kind;
    n.props = std::move(props);

    /* Pull schema from registry to size outputs + determine time_invariant. */
    const task::KindInfo* info = task::lookup(kind);
    if (!info) {
        throw std::runtime_error("graph::Builder::add: unregistered TaskKindId");
    }
    n.time_invariant = info->time_invariant;
    n.cacheable      = info->cacheable;

    /* Inputs — name/type come from schema, source from caller. Enforce arity.
     * Variadic kernels: when info->variadic_last_input is true, the final
     * schema entry describes a repeating port; caller must provide at least
     * (schema.size() - 1) refs, and all refs at index ≥ (schema.size() - 1)
     * share the last schema entry's type. */
    if (info->variadic_last_input) {
        if (info->input_schema.empty()) {
            throw std::runtime_error("graph::Builder::add: variadic kind has empty input_schema");
        }
        const size_t fixed = info->input_schema.size() - 1;
        if (input_refs.size() < fixed) {
            throw std::runtime_error("graph::Builder::add: input count below fixed prefix");
        }
    } else if (input_refs.size() != info->input_schema.size()) {
        throw std::runtime_error("graph::Builder::add: input count mismatch");
    }
    n.inputs.reserve(input_refs.size());
    for (size_t i = 0; i < input_refs.size(); ++i) {
        InputPort p;
        const size_t schema_idx =
            (info->variadic_last_input && i >= info->input_schema.size() - 1)
                ? info->input_schema.size() - 1
                : i;
        p.name   = info->input_schema[schema_idx].name;
        p.type   = info->input_schema[schema_idx].type;
        p.source = input_refs[i];
        n.inputs.push_back(std::move(p));
    }

    /* Outputs — declared by schema. */
    n.outputs.reserve(info->output_schema.size());
    for (const auto& op : info->output_schema) {
        OutputPort o;
        o.name = op.name;
        o.type = op.type;
        n.outputs.push_back(std::move(o));
    }

    NodeId id{static_cast<uint32_t>(draft_nodes_.size())};
    draft_nodes_.push_back(std::move(n));
    return id;
}

void Graph::Builder::name_terminal(std::string name, PortRef port) {
    named_terminals_.emplace_back(std::move(name), port);
}

Graph Graph::Builder::build() && {
    Graph g;
    const size_t n = draft_nodes_.size();

    /* Kahn's topological sort. Since add() appends in creation order and inputs
     * can only reference earlier NodeIds (we don't yet support forward refs),
     * the natural index order is already a valid topo order. We still do the
     * Kahn pass to validate and to produce the reverse adjacency list. */
    std::vector<uint32_t> indegree(n, 0);
    std::vector<std::vector<NodeId>> dependents(n);

    for (uint32_t i = 0; i < n; ++i) {
        for (const auto& in : draft_nodes_[i].inputs) {
            const uint32_t src = in.source.node.v;
            if (src >= n) {
                throw std::runtime_error("graph::Builder::build: dangling input");
            }
            if (src >= i) {
                throw std::runtime_error("graph::Builder::build: forward ref not supported");
            }
            indegree[i]++;
            dependents[src].push_back(NodeId{i});
        }
    }

    std::queue<NodeId> ready;
    for (uint32_t i = 0; i < n; ++i) if (indegree[i] == 0) ready.push(NodeId{i});

    std::vector<NodeId> topo;
    topo.reserve(n);
    while (!ready.empty()) {
        NodeId u = ready.front(); ready.pop();
        topo.push_back(u);
        for (NodeId v : dependents[u.v]) {
            if (--indegree[v.v] == 0) ready.push(v);
        }
    }
    if (topo.size() != n) {
        throw std::runtime_error("graph::Builder::build: cycle detected");
    }

    /* Content hashes — bottom-up, following topo order. */
    std::vector<uint64_t> per_node_hash(n, 0);
    for (NodeId id : topo) {
        const Node& node = draft_nodes_[id.v];
        std::vector<uint64_t> input_hashes;
        input_hashes.reserve(node.inputs.size());
        for (const auto& in : node.inputs) {
            input_hashes.push_back(per_node_hash[in.source.node.v]);
        }
        per_node_hash[id.v] = hash_node_static(node, input_hashes);
    }

    /* Apply hashes back into nodes + compute overall graph hash. */
    uint64_t gh = kFnv64Offset;
    for (uint32_t i = 0; i < n; ++i) {
        draft_nodes_[i].content_hash = per_node_hash[i];
        gh = fnv_mix_u64(gh, per_node_hash[i]);
    }

    g.nodes_         = std::move(draft_nodes_);
    g.topo_          = std::move(topo);
    g.dependents_    = std::move(dependents);
    g.content_hash_  = gh;
    for (auto& [name, port] : named_terminals_) {
        g.terminals_.emplace(std::move(name), port);
    }
    return g;
}

std::vector<std::string_view> Graph::terminal_names() const {
    std::vector<std::string_view> out;
    out.reserve(terminals_.size());
    for (const auto& [k, _] : terminals_) out.push_back(k);
    return out;
}

std::optional<PortRef> Graph::terminal(std::string_view name) const {
    auto it = terminals_.find(std::string(name));
    if (it == terminals_.end()) return std::nullopt;
    return it->second;
}

void Graph::set_terminal(std::string name, PortRef port) {
    terminals_[std::move(name)] = port;
}

}  // namespace me::graph
