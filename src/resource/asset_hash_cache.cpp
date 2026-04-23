#include "resource/asset_hash_cache.hpp"
#include "resource/content_hash.hpp"

#include <algorithm>
#include <cctype>

namespace me::resource {

namespace {

std::string to_lower(std::string_view s) {
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

}  // namespace

std::string AssetHashCache::get_or_compute(std::string_view uri, std::string* err) {
    /* Fast path: cached. */
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(std::string{uri});
        if (it != map_.end()) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    /* Compute outside the mutex — a 1-GB file's sha256 shouldn't block
     * other engine threads from making lookups. */
    std::string hash = sha256_hex_streaming(uri, err);
    if (hash.empty()) return {};
    {
        std::lock_guard<std::mutex> lk(mtx_);
        /* Someone may have raced us; try_emplace preserves the first
         * computed value, which is fine because the file's hash can't
         * differ between concurrent reads. */
        auto [it, /*inserted*/_] = map_.try_emplace(std::string{uri}, std::move(hash));
        return it->second;
    }
}

void AssetHashCache::seed(std::string_view uri, std::string_view hex_hash) {
    if (hex_hash.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    map_[std::string{uri}] = std::string{hex_hash};
}

std::string AssetHashCache::peek(std::string_view uri) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(std::string{uri});
    return it != map_.end() ? it->second : std::string{};
}

std::size_t AssetHashCache::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return map_.size();
}

std::int64_t AssetHashCache::hit_count() const {
    return hits_.load(std::memory_order_relaxed);
}

std::int64_t AssetHashCache::miss_count() const {
    return misses_.load(std::memory_order_relaxed);
}

void AssetHashCache::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    map_.clear();
}

std::size_t AssetHashCache::invalidate_by_hash(std::string_view hex_hash) {
    const std::string target = to_lower(hex_hash);
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->second == target) {
            it = map_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

}  // namespace me::resource
