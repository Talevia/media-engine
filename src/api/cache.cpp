#include "media_engine/cache.h"
#include "core/engine_impl.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/codec_pool.hpp"
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
    /* Disk cache lives with the M6 frame server; the `cache_dir` config
     * still flows through but the backing store isn't wired yet — report
     * "unlimited, nothing used" until that lands. */
    out->disk_bytes_used  = 0;
    out->disk_bytes_limit = engine->config.cache_dir ? -1 : -1;

    if (engine->asset_hashes) {
        out->entry_count = static_cast<int64_t>(engine->asset_hashes->size());
        out->hit_count   = engine->asset_hashes->hit_count();
        out->miss_count  = engine->asset_hashes->miss_count();
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
    /* CodecPool / GpuContext have no per-entry state today; revisit with
     * codec-pool-impl. */
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
    /* FramePool doesn't currently key by asset content_hash (no frame cache
     * yet); when M6 frame cache lands it should also respond to this call. */
    return ME_OK;
}
