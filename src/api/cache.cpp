#include "media_engine/cache.h"
#include "core/engine_impl.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/codec_pool.hpp"
#include "resource/disk_cache.hpp"
#include "resource/frame_pool.hpp"

#include <cstring>
#include <string>
#include <string_view>

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

        /* Disk usage comes from DiskCache's running counter (seeded
         * at ctor from the existing .bin footprint, updated per
         * put / invalidate / clear / LRU evict). Previously this
         * was a per-call directory scan — the debt-dual-disk-counter
         * cycle unified the two sources so stats can't drift from
         * the enforcement path. */
        if (engine->disk_cache->enabled()) {
            out->disk_bytes_used = engine->disk_cache->disk_bytes_used();
        }
    }
    out->hit_count  = hits;
    out->miss_count = misses;

    /* disk_bytes_limit contract: -1 = unlimited; positive = cap.
     * DiskCache stores 0 for "unlimited"; map to -1 for the C
     * API's sentinel. */
    if (engine->disk_cache && engine->disk_cache->enabled()) {
        const int64_t limit = engine->disk_cache->disk_bytes_limit();
        out->disk_bytes_limit = (limit > 0) ? limit : -1;
    } else {
        out->disk_bytes_limit = -1;
    }

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
