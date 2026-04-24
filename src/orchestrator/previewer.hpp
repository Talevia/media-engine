/*
 * Previewer — single-frame-at-time pull for scrubbing / preview UI.
 *
 * Holds a borrowed Timeline + the engine pointer (for AssetHashCache
 * / CodecPool / DiskCache access). `frame_at(time)` walks the
 * bottom track's active clip at `time`, opens a decoder via the
 * engine's CodecPool, seeks frame-accurate in source-stream
 * coordinates, converts the decoded frame to tightly-packed RGBA8,
 * and returns a caller-owned `me_frame`. DiskCache round-trips are
 * transparent — repeat fetches at the same (asset_hash, source_t)
 * key hit cache without re-decoding.
 *
 * Phase-1 compose: single-track only. Multi-track composite-through-
 * preview arrives when Previewer grows the full compose kernel path
 * (separate bullet once a consumer pins the need).
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

    /* Render a single frame at timeline-coordinate `time` (rational
     * seconds). The caller owns `*out_frame` and must release via
     * me_frame_destroy. Returns ME_E_NOT_FOUND when `time` is past
     * the timeline end or outside every clip's time range; other
     * non-ME_OK statuses propagate from the decoder / sws path. */
    me_status_t frame_at(me_rational_t time, me_frame** out_frame);

private:
    me_engine*                       engine_;
    std::shared_ptr<const Timeline>  tl_;
    /* Populated once the graph-eval compose path lands — today's
     * single-track Previewer talks directly to the CodecPool. */
    [[maybe_unused]] SegmentCache    graph_cache_;
};

}  // namespace me::orchestrator
