/*
 * Previewer — single-frame-at-time pull for scrubbing / preview UI.
 *
 * Holds a borrowed Timeline + the engine pointer. `frame_at(time)`
 * resolves the bottom track's active clip at `time`, compiles a
 * three-node graph (IoDemux → IoDecodeVideo → RenderConvertRgba8),
 * and runs it through `engine.scheduler.evaluate_port`. Two cache
 * layers wrap the call: cross-process `DiskCache` (peek + write-
 * through, surfaced via me_cache_*) and in-process scheduler
 * `OutputCache` (transparent to the call site). Returns a caller-
 * owned `me_frame`.
 *
 * Phase-1 compose: single-track only. Multi-track composite arrives
 * when RenderComposeCpu kernel + multi-input graph topology land —
 * see BACKLOG `previewer-multi-track-compose-graph`.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"

#include <memory>

struct me_engine;
struct me_frame;

namespace me::orchestrator {

class Previewer {
public:
    Previewer(me_engine* engine, std::shared_ptr<const Timeline> timeline)
        : engine_(engine), tl_(std::move(timeline)) {}

    /* Render a single frame at timeline-coordinate `time` (rational
     * seconds). The caller owns `*out_frame` and must release via
     * me_frame_destroy. Returns ME_E_NOT_FOUND when `time` is past
     * the timeline end or outside every clip's time range; other
     * non-ME_OK statuses propagate from the decoder / sws path. */
    me_status_t frame_at(me_rational_t time, me_frame** out_frame);

private:
    me_engine*                       engine_;
    std::shared_ptr<const Timeline>  tl_;
};

}  // namespace me::orchestrator
