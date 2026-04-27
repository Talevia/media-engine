/*
 * Thumbnail generation.
 *
 * Produces a PNG of the frame at `time` in the media at `uri`. If max_width
 * or max_height are non-zero, the frame is scaled to fit within that box
 * preserving aspect ratio.
 */
#ifndef MEDIA_ENGINE_THUMBNAIL_H
#define MEDIA_ENGINE_THUMBNAIL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Out buffer is allocated by the engine; caller must free with me_buffer_free. */
ME_API me_status_t me_thumbnail_png(
    me_engine_t*  engine,
    const char*   uri,
    me_rational_t time,
    int           max_width,   /* 0 = native */
    int           max_height,  /* 0 = native */
    uint8_t**     out_png,
    size_t*       out_size);

ME_API void me_buffer_free(uint8_t* buf);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_THUMBNAIL_H */
