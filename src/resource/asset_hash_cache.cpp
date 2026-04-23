#include "resource/asset_hash_cache.hpp"
#include "resource/content_hash.hpp"

namespace me::resource {

std::string AssetHashCache::get_or_compute(std::string_view uri, std::string* err) {
    /* Fast path: cached. */
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(std::string{uri});
        if (it != map_.end()) return it->second;
    }
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

}  // namespace me::resource
