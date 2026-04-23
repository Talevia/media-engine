/*
 * AssetHashCache — engine-wide URI → SHA-256 hex cache.
 *
 * `get_or_compute` hashes a file only on first miss per URI. `seed` lets
 * the timeline loader hand in a trusted hash from the JSON so we skip the
 * compute. Thread-safe: the hash is computed outside the mutex so long
 * files don't block other lookups.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace me::resource {

class AssetHashCache {
public:
    /* Return cached hex sha256 if known. Otherwise stream the file, cache,
     * and return. On error returns empty string and writes *err when
     * non-null. */
    std::string get_or_compute(std::string_view uri, std::string* err = nullptr);

    /* Seed a known hash (e.g. from JSON). No-op if hash is empty. */
    void seed(std::string_view uri, std::string_view hex_hash);

    /* Lookup without compute. Empty string if not cached. */
    std::string peek(std::string_view uri) const;

    std::size_t size()       const;
    std::int64_t hit_count()  const;
    std::int64_t miss_count() const;

    /* Drop every cached entry. Used by me_cache_clear. Counters survive
     * (they track cumulative hit/miss across the engine's lifetime). */
    void clear();

    /* Drop every entry whose hex value equals `hex_hash`. Returns how many
     * were removed. Used by me_cache_invalidate_asset — the content_hash
     * argument maps directly to stored values. `hex_hash` is matched
     * case-insensitively against the normalized lowercase keys. */
    std::size_t invalidate_by_hash(std::string_view hex_hash);

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::string> map_;
    mutable std::atomic<std::int64_t> hits_{0};
    mutable std::atomic<std::int64_t> misses_{0};
};

}  // namespace me::resource
