#include "media_engine/cache.h"

#include <cstring>

/* Cache machinery is mostly an M6 concern (disk cache + invalidation).
 * me_cache_stats is an M1 exit criterion — see cache-stats-impl. Each
 * function below is annotated with a STUB: <slug> marker so the
 * tools/check_stubs.sh inventory picks them up as unimplemented. */

extern "C" me_status_t me_cache_stats(me_engine_t*, me_cache_stats_t* out) {
    /* STUB: cache-stats-impl — returns zeroed counters; wire to FramePool / AssetHashCache / CodecPool. */
    if (!out) return ME_E_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->disk_bytes_limit = -1;
    return ME_OK;
}

extern "C" me_status_t me_cache_clear(me_engine_t*) {
    /* STUB: cache-clear-impl — M6 disk cache; currently a no-op. */
    return ME_OK;
}

extern "C" me_status_t me_cache_invalidate_asset(me_engine_t*, const char*) {
    /* STUB: cache-invalidate-impl — M6 disk cache; currently a no-op. */
    return ME_OK;
}
