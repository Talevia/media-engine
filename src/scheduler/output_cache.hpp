/*
 * me::sched::OutputCache — in-process LRU cache for graph::OutputSlot
 * values, keyed by (content_hash, port_idx) for time-invariant nodes
 * or hash_combine(content_hash, EvalContext.time, port_idx) for
 * time-dependent ones.
 *
 * The "Cache" referenced by EvalContext::cache and TaskContext::cache
 * is this class — those two fields used to be a forward-declared
 * `class Cache` with no definition; this is the definition.
 *
 * See docs/ARCHITECTURE_GRAPH.md §缓存集成 for the contract:
 *
 *   key = node.time_invariant
 *       ? node.content_hash
 *       : hash_combine(node.content_hash, eval_ctx.time)
 *   if (auto cached = cache.get(key, port_idx); cached)
 *       eval->resolve(...);  // skip kernel
 *
 * Threading: every public method takes `mu_`; concurrent get/put from
 * different EvalInstances is safe. LRU is updated on every get hit
 * (write-mutates the access list), so we use a plain mutex rather
 * than a shared_mutex — there's no read-only fast path.
 *
 * Bounded capacity: configured at construction. When `put` would
 * exceed capacity, the oldest entry by access order is evicted.
 * Capacity = 0 disables the cache (every get returns nullopt; put
 * is a no-op). This keeps the API uniform: scheduler can always
 * write to ctx.cache, and turn-off is "set capacity to 0".
 */
#pragma once

#include "graph/types.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace me::sched {

class OutputCache {
public:
    /* Variant stored per (key, port_idx) — same shape as
     * graph::OutputSlot::v. Aliased here so the scheduler can write
     * `cache->put(key, idx, std::move(slot.v))` without spelling out
     * the variant. */
    using Variant = decltype(graph::OutputSlot::v);

    /* `capacity` is the maximum number of entries; 0 disables the
     * cache entirely (get always misses, put is no-op). */
    explicit OutputCache(std::size_t capacity = 0);

    OutputCache(const OutputCache&)            = delete;
    OutputCache& operator=(const OutputCache&) = delete;

    /* Returns the cached value if present; touches LRU recency.
     * Returns nullopt on miss or when capacity is 0. */
    std::optional<Variant> get(uint64_t cache_key, uint8_t port_idx);

    /* Inserts or refreshes an entry. Evicts the oldest entry if
     * insertion would exceed capacity. No-op when capacity is 0. */
    void put(uint64_t cache_key, uint8_t port_idx, Variant v);

    /* Diagnostic: number of live entries. */
    std::size_t size() const noexcept;

    /* Diagnostic: cumulative hit / miss counters since construction
     * (includes capacity-0 misses). */
    std::int64_t hit_count()  const noexcept;
    std::int64_t miss_count() const noexcept;

    /* Drop everything. Counters survive. */
    void clear();

private:
    using Key = std::pair<uint64_t, uint8_t>;

    struct PairHash {
        std::size_t operator()(const Key& k) const noexcept {
            /* xxhash-style composite of the 64-bit content hash and
             * the 8-bit port idx — port_idx is 0..N for small N so
             * we mix it with a prime to spread the low bits. */
            return static_cast<std::size_t>(
                k.first ^ (static_cast<uint64_t>(k.second) * 0x9E3779B97F4A7C15ULL));
        }
    };

    struct Entry {
        Key     key;
        Variant value;
    };

    mutable std::mutex                                              mu_;
    std::list<Entry>                                                lru_;
    std::unordered_map<Key, std::list<Entry>::iterator, PairHash>   index_;
    std::size_t                                                     capacity_;
    /* Counters intentionally live under `mu_` (touched on every
     * get/put) — no need for atomics, and keeping them in the
     * critical section dodges torn-read questions. */
    std::int64_t                                                    hits_   = 0;
    std::int64_t                                                    misses_ = 0;
};

/* Mix a me_rational_t time into a content hash to derive the cache
 * key for time-dependent nodes. Stable per process; not used as a
 * persistent identifier. */
uint64_t mix_time_into_hash(uint64_t base, me_rational_t time) noexcept;

}  // namespace me::sched
