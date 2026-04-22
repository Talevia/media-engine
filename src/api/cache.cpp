#include "media_engine/cache.h"

#include <cstring>

/* Stub: cache machinery is added alongside the frame server. */

extern "C" me_status_t me_cache_stats(me_engine_t*, me_cache_stats_t* out) {
    if (!out) return ME_E_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));
    out->disk_bytes_limit = -1;
    return ME_OK;
}

extern "C" me_status_t me_cache_clear(me_engine_t*) {
    return ME_OK;
}

extern "C" me_status_t me_cache_invalidate_asset(me_engine_t*, const char*) {
    return ME_OK;
}
