/*
 * Timeline loading.
 *
 * A timeline is an immutable object parsed from JSON. See TIMELINE_SCHEMA.md
 * for the schema contract. Timelines are cheap to hold and reference
 * arbitrarily many assets by URI; assets themselves are opened on demand
 * during render.
 */
#ifndef MEDIA_ENGINE_TIMELINE_H
#define MEDIA_ENGINE_TIMELINE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse timeline JSON.
 *
 * json does not need to be NUL-terminated; len is authoritative.
 * Returns ME_E_PARSE on malformed JSON, ME_E_INVALID_ARG on schema
 * violations. Use me_engine_last_error for details. */
ME_API me_status_t me_timeline_load_json(
    me_engine_t*    engine,
    const char*     json,
    size_t          len,
    me_timeline_t** out);

ME_API void me_timeline_destroy(me_timeline_t* tl);

/* --- Queries -------------------------------------------------------------- */

ME_API me_rational_t me_timeline_duration(const me_timeline_t* tl);
ME_API me_rational_t me_timeline_frame_rate(const me_timeline_t* tl);
ME_API void          me_timeline_resolution(const me_timeline_t* tl, int* width, int* height);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_TIMELINE_H */
