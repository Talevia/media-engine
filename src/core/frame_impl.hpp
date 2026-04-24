/* Internal — me_frame body (opaque via public header).
 *
 * Populated by `me::orchestrator::Previewer::frame_at`; consumed
 * by `me_frame_*` C API accessors in src/api/render.cpp.
 * RGBA8 row-major, stride == width × 4 always (simpler consumer
 * contract than a variable pitch — Previewer's sws_scale path
 * produces tightly-packed RGBA anyway). */
#pragma once

#include <cstdint>
#include <vector>

struct me_frame {
    int                  width  = 0;
    int                  height = 0;
    int                  stride = 0;      /* bytes = width * 4 */
    std::vector<uint8_t> rgba;             /* width * height * 4 */
};
