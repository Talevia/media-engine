#include "scheduler/output_cache.hpp"

#include <utility>

namespace me::sched {

OutputCache::OutputCache(std::size_t capacity)
    : capacity_(capacity) {}

std::optional<OutputCache::Variant>
OutputCache::get(uint64_t cache_key, uint8_t port_idx) {
    std::lock_guard<std::mutex> lk(mu_);
    if (capacity_ == 0) {
        ++misses_;
        return std::nullopt;
    }
    auto it = index_.find({cache_key, port_idx});
    if (it == index_.end()) {
        ++misses_;
        return std::nullopt;
    }
    /* Touch LRU recency: splice the hit entry to the front. */
    lru_.splice(lru_.begin(), lru_, it->second);
    ++hits_;
    /* Return a copy of the variant. shared_ptr arms inside copy cheaply
     * (refcount bump); scalars / strings copy. */
    return it->second->value;
}

void OutputCache::put(uint64_t cache_key, uint8_t port_idx, Variant v) {
    std::lock_guard<std::mutex> lk(mu_);
    if (capacity_ == 0) return;

    const Key k{cache_key, port_idx};
    if (auto it = index_.find(k); it != index_.end()) {
        /* Existing entry: refresh value + move to front. */
        it->second->value = std::move(v);
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    /* Evict from the back until we have room for the new entry. */
    while (lru_.size() >= capacity_) {
        const Key& evict_key = lru_.back().key;
        index_.erase(evict_key);
        lru_.pop_back();
    }

    lru_.push_front(Entry{k, std::move(v)});
    index_.emplace(k, lru_.begin());
}

std::size_t OutputCache::size() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return lru_.size();
}

std::int64_t OutputCache::hit_count() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return hits_;
}

std::int64_t OutputCache::miss_count() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return misses_;
}

void OutputCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lru_.clear();
    index_.clear();
}

uint64_t mix_time_into_hash(uint64_t base, me_rational_t time) noexcept {
    /* FNV-1a-style mix of (num, den) into the base. Stable per process;
     * not used as a persistent identifier. */
    constexpr uint64_t prime = 0x100000001b3ULL;
    uint64_t h = base;
    h ^= static_cast<uint64_t>(time.num);
    h *= prime;
    h ^= static_cast<uint64_t>(time.den);
    h *= prime;
    return h;
}

}  // namespace me::sched
