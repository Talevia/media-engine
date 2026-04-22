/*
 * media-engine — shared types, status codes, handles.
 *
 * This header is C-only; keep it ABI-stable. See docs/API.md for the
 * C API contract (ownership, threading, error handling).
 */
#ifndef MEDIA_ENGINE_TYPES_H
#define MEDIA_ENGINE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Opaque handles -------------------------------------------------------
 * Always used as pointers. Struct bodies are engine-internal. */
typedef struct me_engine      me_engine_t;
typedef struct me_timeline    me_timeline_t;
typedef struct me_render_job  me_render_job_t;
typedef struct me_frame       me_frame_t;
typedef struct me_media_info  me_media_info_t;

/* --- Status codes --------------------------------------------------------
 * 0 = success; negative = failure. Names are stable ABI. */
typedef enum me_status {
    ME_OK                = 0,
    ME_E_INVALID_ARG     = -1,
    ME_E_OUT_OF_MEMORY   = -2,
    ME_E_IO              = -3,
    ME_E_PARSE           = -4,
    ME_E_DECODE          = -5,
    ME_E_ENCODE          = -6,
    ME_E_UNSUPPORTED     = -7,
    ME_E_CANCELLED       = -8,
    ME_E_NOT_FOUND       = -9,
    ME_E_INTERNAL        = -100
} me_status_t;

/* Short English description. Never NULL. Static storage. */
const char* me_status_str(me_status_t status);

/* --- Version -------------------------------------------------------------- */
typedef struct me_version {
    int         major;
    int         minor;
    int         patch;
    const char* git_sha;  /* static storage; empty string if unknown */
} me_version_t;

me_version_t me_version(void);

/* --- Rational number -----------------------------------------------------
 * Used for time and frame rate. den MUST be > 0 when produced by the engine;
 * callers SHOULD also pass den > 0. Time is in seconds (num/den). */
typedef struct me_rational {
    int64_t num;
    int64_t den;
} me_rational_t;

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_TYPES_H */
