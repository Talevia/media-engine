/* AssetCache impl. See header for the contract.
 *
 * Hash mixer is FNV-1a (same family as
 * `me::sched::mix_time_into_hash`); 64-bit prime + xor-then-multiply
 * loop walks every byte of every input tensor. Sufficient for an
 * LRU bucketing key — collisions just mean "we'll occasionally
 * recompute when we could have hit"; correctness depends on the
 * full key (model identity strings + input_hash) compared via
 * `operator==`, not just the hash bucket.
 */
#include "inference/asset_cache.hpp"

#include <cstring>
#include <utility>

namespace me::inference {

namespace {

constexpr std::uint64_t kFnvPrime  = 0x100000001b3ULL;
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;

inline std::uint64_t fnv1a_step(std::uint64_t h, std::uint8_t b) noexcept {
    h ^= static_cast<std::uint64_t>(b);
    h *= kFnvPrime;
    return h;
}

std::uint64_t fnv1a_bytes(std::uint64_t h, const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) h = fnv1a_step(h, p[i]);
    return h;
}

std::uint64_t fnv1a_string(std::uint64_t h, const std::string& s) noexcept {
    /* Length-prefix so {"a","b"} doesn't collide with {"ab"}. */
    const std::uint64_t len = s.size();
    h = fnv1a_bytes(h, &len, sizeof(len));
    return fnv1a_bytes(h, s.data(), s.size());
}

}  // namespace

std::size_t AssetCacheKeyHash::operator()(const AssetCacheKey& k) const noexcept {
    /* Compose the 4 fields into a 64-bit hash; std::size_t cast at
     * the end. Same FNV-1a family as the input-bytes hasher so the
     * full-key xor-mult progression is uniform. */
    std::uint64_t h = kFnvOffset;
    h = fnv1a_string(h, k.model_id);
    h = fnv1a_string(h, k.model_version);
    h = fnv1a_string(h, k.quantization);
    h = fnv1a_bytes(h, &k.input_hash, sizeof(k.input_hash));
    return static_cast<std::size_t>(h);
}

std::uint64_t hash_inputs(const std::map<std::string, Tensor>& inputs) noexcept {
    /* Iterate in std::map sorted order so the same logical input
     * set produces the same hash regardless of insertion order. */
    std::uint64_t h = kFnvOffset;
    for (const auto& [name, t] : inputs) {
        h = fnv1a_string(h, name);
        const auto dt = static_cast<std::uint8_t>(t.dtype);
        h = fnv1a_step(h, dt);
        const std::uint64_t shape_len = t.shape.size();
        h = fnv1a_bytes(h, &shape_len, sizeof(shape_len));
        for (auto dim : t.shape) {
            h = fnv1a_bytes(h, &dim, sizeof(dim));
        }
        const std::uint64_t bytes_len = t.bytes.size();
        h = fnv1a_bytes(h, &bytes_len, sizeof(bytes_len));
        h = fnv1a_bytes(h, t.bytes.data(), t.bytes.size());
    }
    return h;
}

AssetCache::AssetCache(std::size_t capacity) : capacity_(capacity) {}

std::optional<std::map<std::string, Tensor>> AssetCache::get(const AssetCacheKey& key) {
    std::lock_guard<std::mutex> lk(mu_);
    if (capacity_ == 0) {
        ++misses_;
        return std::nullopt;
    }
    auto it = index_.find(key);
    if (it == index_.end()) {
        ++misses_;
        return std::nullopt;
    }
    /* LRU: move the entry to the front on hit. */
    lru_.splice(lru_.begin(), lru_, it->second);
    ++hits_;
    /* Return a deep copy — caller mutates freely without trampling
     * the cache's stored bytes. */
    return it->second->value;
}

void AssetCache::put(AssetCacheKey key, std::map<std::string, Tensor> outputs) {
    std::lock_guard<std::mutex> lk(mu_);
    if (capacity_ == 0) return;
    auto it = index_.find(key);
    if (it != index_.end()) {
        /* Refresh existing entry: replace value + move to front. */
        it->second->value = std::move(outputs);
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    /* Evict if at capacity. */
    while (lru_.size() >= capacity_ && !lru_.empty()) {
        const auto& victim = lru_.back();
        index_.erase(victim.key);
        lru_.pop_back();
    }
    lru_.push_front(Entry{std::move(key), std::move(outputs)});
    index_.emplace(lru_.front().key, lru_.begin());
}

std::size_t AssetCache::size() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return lru_.size();
}

std::int64_t AssetCache::hit_count() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return hits_;
}

std::int64_t AssetCache::miss_count() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return misses_;
}

void AssetCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lru_.clear();
    index_.clear();
}

}  // namespace me::inference
