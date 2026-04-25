/*
 * Rendering.
 *
 * Two modes:
 *
 *   Batch  : me_render_start → emits progress via callback → writes file.
 *            Use for final export / pre-compose / offline artifacts.
 *
 *   Frame  : me_render_frame → synchronous, returns a single RGBA8 frame
 *            at the requested time. Use for scrubbing, thumbnails,
 *            agent-driven preview, etc.
 *
 * Both modes read from the same effect graph; cached intermediate results
 * are shared. See VISION §3.3 for the cache contract.
 */
#ifndef MEDIA_ENGINE_RENDER_H
#define MEDIA_ENGINE_RENDER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Batch render --------------------------------------------------------- */

typedef struct me_output_spec {
    const char* path;              /* output file path, required */
    const char* container;         /* "mp4","mov","mkv" — required */

    /* Codec names. "passthrough" means stream-copy when inputs are compatible.
     * Engine validates against its codec registry; unknown names => ME_E_UNSUPPORTED. */
    const char* video_codec;       /* e.g., "h264","hevc","prores","passthrough" */
    const char* audio_codec;       /* e.g., "aac","pcm_s16le","passthrough" */

    int64_t video_bitrate_bps;     /* 0 = codec default */
    int64_t audio_bitrate_bps;     /* 0 = codec default */

    /* Output resolution / frame rate. If width/height == 0 or
     * frame_rate.den == 0, inherit from timeline. */
    int           width;
    int           height;
    me_rational_t frame_rate;
} me_output_spec_t;

typedef enum me_progress_kind {
    ME_PROGRESS_STARTED   = 0,
    ME_PROGRESS_FRAMES    = 1,  /* periodic updates during render */
    ME_PROGRESS_COMPLETED = 2,
    ME_PROGRESS_FAILED    = 3
} me_progress_kind_t;

typedef struct me_progress_event {
    me_progress_kind_t kind;
    float              ratio;        /* [0,1]; valid when kind == FRAMES */
    const char*        message;      /* optional; lifetime = callback scope */
    const char*        output_path;  /* valid when kind == COMPLETED */
    me_status_t        status;       /* valid when kind == FAILED */
} me_progress_event_t;

typedef void (*me_progress_cb)(const me_progress_event_t* ev, void* user);

/* Start an asynchronous render. Returns immediately; work runs on the
 * engine's worker threads. cb is invoked on an engine-owned thread
 * (NOT the caller's). user is passed back verbatim.
 *
 * Caller owns out_job; call me_render_job_destroy after the job reaches
 * a terminal state (COMPLETED / FAILED / cancelled). */
me_status_t me_render_start(
    me_engine_t*            engine,
    const me_timeline_t*    timeline,
    const me_output_spec_t* output,
    me_progress_cb          cb,
    void*                   user,
    me_render_job_t**       out_job);

/* Request cancellation. The job will eventually emit ME_PROGRESS_FAILED
 * with status == ME_E_CANCELLED. */
me_status_t me_render_cancel(me_render_job_t* job);

/* Block until the job reaches a terminal state. Returns the terminal
 * status of the job (ME_OK on success). */
me_status_t me_render_wait(me_render_job_t* job);

void me_render_job_destroy(me_render_job_t* job);

/* --- Frame server -------------------------------------------------------- */

/* Render a single frame at `time` (timeline coordinates, rational seconds).
 * Synchronous. Returns an RGBA8 frame owned by the caller.
 *
 * For scrubbing / preview: call repeatedly; the engine's frame cache will
 * serve hits when the underlying graph is unchanged. */
me_status_t me_render_frame(
    me_engine_t*         engine,
    const me_timeline_t* timeline,
    me_rational_t        time,
    me_frame_t**         out_frame);

void           me_frame_destroy(me_frame_t* frame);
int            me_frame_width(const me_frame_t* f);
int            me_frame_height(const me_frame_t* f);

/* Decoded RGBA8 pixels.
 *
 * Layout: row-major, byte order {R, G, B, A} per pixel — byte 0 is
 *   the red channel, byte 3 is alpha. No padding between pixels or
 *   rows: stride is exactly width*4, total length is width*height*4.
 *
 * Origin: top-left. x grows right, y grows down. Pixel (x, y) lives
 *   at byte offset (y * width + x) * 4.
 *
 * Color space: the engine targets the timeline's declared color
 *   space (TIMELINE_SCHEMA.md §colorSpace) — sRGB primaries + sRGB
 *   transfer for SDR Rec.709 by default; HDR (PQ / HLG) returns
 *   linear-light values when enabled by the timeline.
 *
 * Lifetime: pointer is valid until the matching me_frame_destroy.
 *   Hosts that need long-lived ownership must copy. */
const uint8_t* me_frame_pixels(const me_frame_t* f);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_RENDER_H */
