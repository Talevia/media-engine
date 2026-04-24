#include "media_engine/cache.h"
#include "core/engine_impl.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/codec_pool.hpp"
#include "resource/disk_cache.hpp"
#include "resource/frame_pool.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

/* Sum the byte sizes of every `.bin` file under `dir`. Used to
 * populate me_cache_stats_t.disk_bytes_used. Scans the directory
 * per call — cheap for tens-of-thousands of entries; if cache grows
 * beyond that scale, cache the total in DiskCache itself. */
int64_t dir_used_bytes(const std::string& dir) {
    if (dir.empty()) return 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return 0;
    int64_t total = 0;
    for (const auto& entry : it) {
        std::error_code check_ec;
        if (!entry.is_regular_file(check_ec)) continue;
        if (entry.path().extension() != ".bin") continue;
        const auto sz = entry.file_size(check_ec);
        if (!check_ec) total += static_cast<int64_t>(sz);
    }
    return total;
}

}  // namespace

extern "C" me_status_t me_cache_stats(me_engine_t* engine, me_cache_stats_t* out) {
    if (!engine || !out) return ME_E_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));

    if (engine->frames) {
        const auto s = engine->frames->stats();
        out->memory_bytes_used  = s.memory_bytes_used;
        out->memory_bytes_limit = s.memory_bytes_limit;
    }

    /* Hit/miss counters combine asset-level (AssetHashCache — URI →
     * hash lookups) + frame-level (DiskCache — frame-key lookups).
     * Both tiers are conceptually "cache"; summing matches the
     * me_cache_stats_t contract that these fields report the
     * engine's composite cache behaviour per VISION §3.3. */
    int64_t hits = 0, misses = 0;
    if (engine->asset_hashes) {
        hits   += engine->asset_hashes->hit_count();
        misses += engine->asset_hashes->miss_count();
        out->entry_count = static_cast<int64_t>(engine->asset_hashes->size());
    }
    if (engine->disk_cache) {
        hits   += engine->disk_cache->hit_count();
        misses += engine->disk_cache->miss_count();

        /* Disk bytes usage = sum of all .bin file sizes. Only
         * surface this when the cache is enabled; otherwise
         * report 0 / unlimited (-1). */
        if (engine->disk_cache->enabled()) {
            out->disk_bytes_used = dir_used_bytes(engine->disk_cache->dir());
        }
    }
    out->hit_count  = hits;
    out->miss_count = misses;
    /* disk_bytes_limit: -1 = unlimited. Phase-1 has no size cap; a
     * future LRU bullet can populate this from config. */
    out->disk_bytes_limit = -1;

    if (engine->codecs) {
        out->codec_ctx_count = engine->codecs->live_count();
    }
    return ME_OK;
}

extern "C" me_status_t me_cache_clear(me_engine_t* engine) {
    if (!engine) return ME_E_INVALID_ARG;
    if (engine->frames)       engine->frames->reset_counters();
    if (engine->asset_hashes) engine->asset_hashes->clear();
    if (engine->disk_cache) {
        engine->disk_cache->clear();
        engine->disk_cache->reset_counters();
    }
    /* CodecPool has no per-entry state today. */
    return ME_OK;
}

extern "C" me_status_t me_cache_invalidate_asset(me_engine_t* engine, const char* content_hash) {
    if (!engine || !content_hash) return ME_E_INVALID_ARG;

    /* Accept both "sha256:<hex>" and bare "<hex>" — strip the scheme if
     * present. Normalization to lowercase happens inside the cache. */
    std::string_view h{content_hash};
    constexpr std::string_view prefix{"sha256:"};
    if (h.size() >= prefix.size() && h.compare(0, prefix.size(), prefix) == 0) {
        h.remove_prefix(prefix.size());
    }
    if (engine->asset_hashes) {
        engine->asset_hashes->invalidate_by_hash(h);
    }
    /* Cascade to DiskCache — Previewer keys frame entries as
     * `<asset_hash>:<source_time>`, so invalidating by asset_hash
     * prefix removes all cached frames derived from this asset. */
    if (engine->disk_cache) {
        engine->disk_cache->invalidate_by_prefix(std::string(h));
    }
    return ME_OK;
}
