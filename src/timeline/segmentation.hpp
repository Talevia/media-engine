/*
 * Segmentation — split a Timeline into contiguous time ranges within which
 * the set of active clips (and eventually transitions) is constant.
 *
 * Each Segment maps to one compiled Graph (via graph::compile_segment).
 * Orchestrators iterate segments, compile-or-cache one Graph per segment,
 * drive scheduler.evaluate_port for every frame-time within the segment.
 *
 * See docs/ARCHITECTURE_GRAPH.md §批编码 and the timeline-segmentation
 * backlog item.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"

#include <cstdint>
#include <vector>

namespace me::timeline {

/* Stable reference to a clip inside Timeline.clips. */
struct ClipRef {
    uint32_t idx = 0;
    constexpr bool operator==(const ClipRef&) const = default;
};

/* Placeholder — transitions aren't in the IR yet (they arrive with the
 * multi-track / transition backlog). Kept here so downstream code can
 * depend on the final shape of Segment without future breakage. */
struct TransitionRef {
    uint32_t idx = 0;
    constexpr bool operator==(const TransitionRef&) const = default;
};

struct Segment {
    me_rational_t              start;    /* inclusive */
    me_rational_t              end;      /* exclusive */
    std::vector<ClipRef>       active_clips;
    std::vector<TransitionRef> active_transitions;

    /* Stable hash of (active_clips ∪ active_transitions) — used as a cache
     * key by orchestrators when looking up or compiling a Graph for this
     * segment. Does NOT include the time range itself: two disjoint
     * segments with the same active set share a cached Graph. */
    uint64_t                   boundary_hash = 0;
};

/* Slice the Timeline at every clip start/end. Returns segments in
 * ascending time order, covering [0, timeline.duration).
 *
 * Empty timeline (no clips or duration == 0) returns an empty vector.
 * Gaps (times where no clip is active) produce a Segment with
 * active_clips == [].
 */
std::vector<Segment> segment(const Timeline&);

}  // namespace me::timeline
