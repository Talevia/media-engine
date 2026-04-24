/*
 * me::orchestrator::compose_transition_step — extracted Transition
 * branch of the compose frame loop.
 *
 * Scope-A slice of `debt-split-compose-sink-cpp` (compose_sink.cpp
 * was 694 lines, approaching the 700 debt-P0 threshold). The
 * Transition kind handling — pulling two decoders, blending via
 * cross_dissolve, handling from-EOF degradation — is self-contained
 * enough to extract as a free function. Callers pass in working
 * buffers + decoder state; the helper fills `track_rgba` with the
 * blended output + reports back src dims / transform clip idx.
 *
 * Contract:
 *   - ME_OK on success: `track_rgba` filled with W×H×4 blended RGBA8,
 *     `out_src_w/h` set to W/H, `out_transform_clip_idx` set to
 *     to-clip idx (for opacity/transform lookup in the caller).
 *   - ME_E_NOT_FOUND when the whole transition contributes nothing
 *     (both endpoints drained). Caller should `continue` past this
 *     track at this T.
 *   - ME_E_UNSUPPORTED when endpoint frame dims don't match W×H
 *     (phase-1 restriction; Transform-on-transition is a future
 *     bullet).
 *   - Propagated decoder errors from pull_next_video_frame.
 *
 * Phase-1 limitations (kept identical to the pre-extraction code
 * in ComposeSink::process):
 *   - Neither endpoint's per-clip Transform is applied during the
 *     blend (both assumed identity); `transition-with-transform`
 *     bullet tracks this follow-up.
 *   - No from-side caching: if from-decoder drains mid-window,
 *     output degrades to pure to-clip (equivalent to t=1) for the
 *     remainder of the window.
 *   - Frame-accurate to_clip alignment after the window is NOT
 *     maintained; `transition-to-clip-source-time-align` bullet
 *     tracks the seek fix.
 */
#pragma once

#include "compose/active_clips.hpp"
#include "media_engine/types.h"
#include "orchestrator/frame_puller.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace me::orchestrator {

me_status_t compose_transition_step(
    const me::compose::FrameSource& fs,
    TrackDecoderState&              td_from,
    TrackDecoderState&              td_to,
    int                             W,
    int                             H,
    std::vector<std::uint8_t>&      track_rgba,
    std::vector<std::uint8_t>&      from_rgba,
    std::vector<std::uint8_t>&      to_rgba,
    int&                            out_src_w,
    int&                            out_src_h,
    std::size_t&                    out_transform_clip_idx,
    std::string*                    err);

}  // namespace me::orchestrator
