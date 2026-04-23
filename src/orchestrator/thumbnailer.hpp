/*
 * Thumbnailer — single-frame → PNG.
 *
 * Bootstrap: stub. Real impl (seek + decode + scale + PNG encode) lands
 * with the thumbnail-impl backlog item.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

struct me_engine;

namespace me::orchestrator {

class Thumbnailer {
public:
    Thumbnailer(me_engine* engine, std::shared_ptr<const Timeline> timeline)
        : engine_(engine), tl_(std::move(timeline)) {}

    /* Render a PNG. Caller frees *out_png via me_buffer_free. Returns
     * ME_E_UNSUPPORTED until the thumbnail-impl backlog item lands. */
    me_status_t png_at(me_rational_t time,
                       int           max_width,
                       int           max_height,
                       uint8_t**     out_png,
                       size_t*       out_size);

private:
    [[maybe_unused]] me_engine*      engine_;  /* used once thumbnail-impl lands */
    [[maybe_unused]] std::shared_ptr<const Timeline> tl_;
};

}  // namespace me::orchestrator
