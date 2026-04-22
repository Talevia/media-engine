#include "media_engine/timeline.h"

/* Stub: full impl arrives with the timeline JSON loader. */

extern "C" me_status_t me_timeline_load_json(
    me_engine_t*, const char*, size_t, me_timeline_t** out) {
    if (out) *out = nullptr;
    return ME_E_UNSUPPORTED;
}

extern "C" void me_timeline_destroy(me_timeline_t*) {
    /* no-op for stub */
}

extern "C" me_rational_t me_timeline_duration(const me_timeline_t*) {
    return me_rational_t{0, 1};
}

extern "C" me_rational_t me_timeline_frame_rate(const me_timeline_t*) {
    return me_rational_t{30, 1};
}

extern "C" void me_timeline_resolution(const me_timeline_t*, int* width, int* height) {
    if (width)  *width  = 0;
    if (height) *height = 0;
}
