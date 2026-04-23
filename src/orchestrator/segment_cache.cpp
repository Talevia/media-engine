#include "orchestrator/segment_cache.hpp"

namespace me::orchestrator {

std::shared_ptr<graph::Graph> SegmentCache::get(uint64_t key) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : it->second;
}

void SegmentCache::insert(uint64_t key, std::shared_ptr<graph::Graph> g) {
    std::lock_guard<std::mutex> lk(mtx_);
    map_[key] = std::move(g);
}

void SegmentCache::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    map_.clear();
}

size_t SegmentCache::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return map_.size();
}

}  // namespace me::orchestrator
