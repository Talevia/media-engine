/*
 * CompositionThumbnailer — single-frame → PNG, *composition-level*.
 *
 * This is the timeline-driven path: given a Timeline + a time, produce a
 * PNG of the rendered composition at that instant. Bootstrap ships as a
 * stub (`ME_E_UNSUPPORTED`); the real impl lands with the M6 frame server
 * (backlog: composition-thumbnail-impl), which is its only consumer.
 *
 * **Not** the asset-level thumbnail path. `me_thumbnail_png(engine, uri, …)`
 * in `src/api/thumbnail.cpp` is a distinct, fully-implemented path that
 * takes a plain URI and doesn't need a Timeline. The two roles are
 * separate on purpose — see `docs/PAIN_POINTS.md` 2026-04-23 for the
 * split rationale.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

struct me_engine;

namespace me::orchestrator {

class CompositionThumbnailer {
public:
    CompositionThumbnailer(me_engine* engine, std::shared_ptr<const Timeline> timeline)
        : engine_(engine), tl_(std::move(timeline)) {}

    /* Render a PNG of the composition at `time`. Caller frees *out_png via
     * me_buffer_free. Returns ME_E_UNSUPPORTED until composition-thumbnail-impl
     * lands (awaits M6 frame server). */
    me_status_t png_at(me_rational_t time,
                       int           max_width,
                       int           max_height,
                       uint8_t**     out_png,
                       size_t*       out_size);

private:
    [[maybe_unused]] me_engine*      engine_;  /* used once composition-thumbnail-impl lands */
    [[maybe_unused]] std::shared_ptr<const Timeline> tl_;
};

}  // namespace me::orchestrator
