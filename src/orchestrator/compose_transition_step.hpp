/*
 * me::orchestrator::compose_transition_step — extracted Transition
 * branch of the compose frame loop.
 *
 * Landed in `debt-split-compose-sink-cpp` as a self-contained helper.
 * `transition-with-transform` cycle extended it to apply each
 * endpoint's `Clip::transform` before cross_dissolve — see contract.
 *
 * Contract:
 *   - ME_OK on success: `track_rgba` filled with W×H×4 blended RGBA8
 *     (per-clip transforms ALREADY APPLIED), `out_src_w/h` = W/H,
 *     `out_transform_clip_idx` = to_clip idx (for caller's opacity
 *     lookup; spatial transform already done — see
 *     `out_spatial_already_applied`).
 *   - `out_spatial_already_applied` = true always (even identity
 *     endpoints pass through affine_blit with identity matrix; the
 *     blit-at-canvas-size makes src/dst dim mismatch a non-issue
 *     during transition and dovetails with src_w=W/out_src_h=H).
 *     Caller must skip its own spatial affine_blit step when this is
 *     true — opacity still applies via alpha_over.
 *   - ME_E_NOT_FOUND when the whole transition contributes nothing
 *     (both endpoints drained). Caller should `continue` past this
 *     track at this T.
 *   - Propagated decoder errors from pull_next_video_frame.
 *
 * Phase-1 limitations (remaining after this cycle):
 *   - No from-side caching: if from-decoder drains mid-window,
 *     output degrades to pure to-clip (equivalent to t=1) for the
 *     remainder of the window.
 *
 * Post-window to_clip realignment: implemented via the
 * `TrackDecoderState::used_as_to_in_transition` flag this helper
 * sets after each successful to_clip pull; compose_sink's
 * SingleClip branch performs a frame-accurate seek to the
 * schema-aligned source_time on the first post-window pull. See
 * `docs/decisions/2026-04-23-transition-to-clip-source-time-align.md`.
 */
#pragma once

#include "compose/active_clips.hpp"
#include "media_engine/types.h"
#include "orchestrator/frame_puller.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace me { struct TransformEvaluated; }

namespace me::orchestrator {

me_status_t compose_transition_step(
    const me::compose::FrameSource& fs,
    const me::TransformEvaluated&   from_tr,     /* evaluated at T */
    bool                            from_has_transform,
    const me::TransformEvaluated&   to_tr,       /* evaluated at T */
    bool                            to_has_transform,
    TrackDecoderState&              td_from,
    TrackDecoderState&              td_to,
    int                             W,
    int                             H,
    std::vector<std::uint8_t>&      track_rgba,
    std::vector<std::uint8_t>&      from_rgba,    /* scratch — source-sized */
    std::vector<std::uint8_t>&      to_rgba,      /* scratch — source-sized */
    std::vector<std::uint8_t>&      from_canvas,  /* scratch — W×H×4 */
    std::vector<std::uint8_t>&      to_canvas,    /* scratch — W×H×4 */
    int&                            out_src_w,
    int&                            out_src_h,
    std::size_t&                    out_transform_clip_idx,
    bool&                           out_spatial_already_applied,
    std::string*                    err);

}  // namespace me::orchestrator
