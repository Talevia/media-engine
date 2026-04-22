#include "media_engine/thumbnail.h"

#include <cstdlib>

/* Stub. */

extern "C" me_status_t me_thumbnail_png(
    me_engine_t*, const char*, me_rational_t, int, int,
    uint8_t** out_png, size_t* out_size) {
    if (out_png)  *out_png  = nullptr;
    if (out_size) *out_size = 0;
    return ME_E_UNSUPPORTED;
}

extern "C" void me_buffer_free(uint8_t* buf) {
    std::free(buf);
}
