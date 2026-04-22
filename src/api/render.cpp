#include "media_engine/render.h"

/* Stubs: real impl arrives with the FFmpeg I/O layer and effect graph. */

extern "C" me_status_t me_render_start(
    me_engine_t*, const me_timeline_t*, const me_output_spec_t*,
    me_progress_cb, void*, me_render_job_t** out_job) {
    if (out_job) *out_job = nullptr;
    return ME_E_UNSUPPORTED;
}

extern "C" me_status_t me_render_cancel(me_render_job_t*) { return ME_E_UNSUPPORTED; }
extern "C" me_status_t me_render_wait(me_render_job_t*)   { return ME_E_UNSUPPORTED; }
extern "C" void        me_render_job_destroy(me_render_job_t*) {}

extern "C" me_status_t me_render_frame(
    me_engine_t*, const me_timeline_t*, me_rational_t, me_frame_t** out_frame) {
    if (out_frame) *out_frame = nullptr;
    return ME_E_UNSUPPORTED;
}

extern "C" void           me_frame_destroy(me_frame_t*)             {}
extern "C" int            me_frame_width(const me_frame_t*)         { return 0; }
extern "C" int            me_frame_height(const me_frame_t*)        { return 0; }
extern "C" const uint8_t* me_frame_pixels(const me_frame_t*)        { return nullptr; }
