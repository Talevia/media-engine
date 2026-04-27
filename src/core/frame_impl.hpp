/* Internal — me_frame body (opaque via public header).
 *
 * Populated by `me_render_frame` (src/api/render.cpp), which wraps
 * the RgbaFrameData produced by `me::orchestrator::compose_frame_at`
 * (src/orchestrator/compose_frame.cpp). Consumed by `me_frame_*` C
 * API accessors in the same file. RGBA8 row-major, stride == width
 * × 4 always (simpler consumer contract than a variable pitch —
 * the per-frame sws_scale path produces tightly-packed RGBA anyway). */
#pragma once

#include <cstdint>
#include <vector>

struct me_frame {
    int                  width  = 0;
    int                  height = 0;
    int                  stride = 0;      /* bytes = width * 4 */
    std::vector<uint8_t> rgba;             /* width * height * 4 */
};
