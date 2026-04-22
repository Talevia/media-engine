/*
 * Engine lifecycle.
 *
 * An engine owns the worker thread pool, GPU context (once implemented),
 * and frame/asset caches. Most other API calls take an engine handle.
 *
 * Engines are thread-safe: multiple threads may share one engine and
 * issue API calls concurrently. Per-handle objects (timelines, render
 * jobs) are single-owner — do not share across threads without external
 * synchronization.
 */
#ifndef MEDIA_ENGINE_ENGINE_H
#define MEDIA_ENGINE_ENGINE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum me_log_level {
    ME_LOG_TRACE = 0,
    ME_LOG_DEBUG = 1,
    ME_LOG_INFO  = 2,
    ME_LOG_WARN  = 3,
    ME_LOG_ERROR = 4,
    ME_LOG_OFF   = 5
} me_log_level_t;

typedef struct me_engine_config {
    /* Worker threads for decode / filter / encode. 0 = hardware_concurrency(). */
    int            num_worker_threads;
    me_log_level_t log_level;

    /* Optional disk cache dir for intermediate frames. NULL = no disk cache. */
    const char* cache_dir;

    /* In-memory frame cache cap, bytes. 0 = engine default. */
    int64_t memory_cache_bytes;
} me_engine_config_t;

/* Create an engine. Pass NULL config for defaults. */
me_status_t me_engine_create(const me_engine_config_t* config, me_engine_t** out);

void me_engine_destroy(me_engine_t* engine);

/* Last error message on this engine (thread-local).
 * Returns a NUL-terminated string, valid until the next engine API call on
 * the calling thread. Empty string if no error has been recorded. */
const char* me_engine_last_error(const me_engine_t* engine);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_ENGINE_H */
