/*
 * Cache observability.
 *
 * The engine caches decoded frames, rendered intermediate RTTs, and
 * probe results keyed by (asset content hash, effect parameters, engine
 * version). These APIs let hosts observe and invalidate.
 *
 * See VISION §3.3.
 */
#ifndef MEDIA_ENGINE_CACHE_H
#define MEDIA_ENGINE_CACHE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct me_cache_stats {
    int64_t memory_bytes_used;
    int64_t memory_bytes_limit;
    int64_t disk_bytes_used;
    int64_t disk_bytes_limit;   /* -1 = unlimited */
    int64_t entry_count;
    int64_t hit_count;
    int64_t miss_count;
} me_cache_stats_t;

me_status_t me_cache_stats(me_engine_t* engine, me_cache_stats_t* out);
me_status_t me_cache_clear(me_engine_t* engine);

/* Drop all cache entries whose inputs transitively reference the asset
 * identified by content_hash. */
me_status_t me_cache_invalidate_asset(me_engine_t* engine, const char* content_hash);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_CACHE_H */
