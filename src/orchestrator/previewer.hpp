/*
 * Previewer — single-frame-at-time pull for scrubbing / preview UI.
 *
 * Bootstrap: holds Timeline + SegmentCache; frame_at() returns
 * ME_E_UNSUPPORTED until compose kernels + frame server (M6) land.
 */
#pragma once

#include "media_engine/types.h"
#include "orchestrator/segment_cache.hpp"
#include "timeline/timeline_impl.hpp"

#include <memory>

struct me_engine;
struct me_frame;

namespace me::orchestrator {

class Previewer {
public:
    Previewer(me_engine* engine, std::shared_ptr<const Timeline> timeline)
        : engine_(engine), tl_(std::move(timeline)) {}

    /* Render a single frame. Currently returns ME_E_UNSUPPORTED; awaits
     * compose kernels + resource::FramePool uploads (M2/M3) and frame
     * server cache (M6). */
    me_status_t frame_at(me_rational_t time, me_frame** out_frame);

private:
    [[maybe_unused]] me_engine*      engine_;  /* used once graph eval lands */
    [[maybe_unused]] std::shared_ptr<const Timeline> tl_;
    [[maybe_unused]] SegmentCache    graph_cache_;
};

}  // namespace me::orchestrator
