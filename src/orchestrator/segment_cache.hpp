/*
 * SegmentCache — maps timeline::Segment.boundary_hash → compiled Graph.
 *
 * Bootstrap scope: just the type + a thread-safe map. Used by Exporter
 * (and a future multi-track variant of `compile_frame_graph`) to avoid
 * recompiling the same graph structure for every frame within a
 * segment, and across segments whose active sets match.
 *
 * Currently per-orchestrator (no cross-orchestrator sharing, per plan
 * decision 2026-04-22-architecture-graph.md).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace me::graph { class Graph; }

namespace me::orchestrator {

class SegmentCache {
public:
    std::shared_ptr<graph::Graph> get(uint64_t boundary_hash) const;
    void insert(uint64_t boundary_hash, std::shared_ptr<graph::Graph>);
    void clear();
    size_t size() const;

private:
    mutable std::mutex mtx_;
    std::unordered_map<uint64_t, std::shared_ptr<graph::Graph>> map_;
};

}  // namespace me::orchestrator
