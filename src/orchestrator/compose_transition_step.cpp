#include "orchestrator/compose_transition_step.hpp"

#include "compose/cross_dissolve.hpp"
#include "compose/frame_convert.hpp"
#include "io/demux_context.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <cstring>

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
    std::string*                    err) {

    /* Pull from_clip; if exhausted, degrade to to-only single-clip
     * rendering (weight-based soft degrade isn't possible without a
     * cached last-from frame — follow-up for handle support). */
    const me_status_t pull_from = pull_next_video_frame(
        td_from.demux->fmt, td_from.video_stream_idx,
        td_from.dec.get(), td_from.pkt_scratch.get(),
        td_from.frame_scratch.get(), err);
    bool from_valid = false;
    int  from_w = 0, from_h = 0;
    if (pull_from == ME_OK) {
        from_w = td_from.frame_scratch->width;
        from_h = td_from.frame_scratch->height;
        if (auto s = me::compose::frame_to_rgba8(
                td_from.frame_scratch.get(), from_rgba, err);
            s != ME_OK) {
            av_frame_unref(td_from.frame_scratch.get());
            return s;
        }
        av_frame_unref(td_from.frame_scratch.get());
        from_valid = true;
    } else if (pull_from != ME_E_NOT_FOUND) {
        return pull_from;
    }

    /* Pull to_clip. Both endpoints drained → whole transition
     * contributes nothing at this T. */
    const me_status_t pull_to = pull_next_video_frame(
        td_to.demux->fmt, td_to.video_stream_idx,
        td_to.dec.get(), td_to.pkt_scratch.get(),
        td_to.frame_scratch.get(), err);
    if (pull_to == ME_E_NOT_FOUND) return ME_E_NOT_FOUND;
    if (pull_to != ME_OK) return pull_to;

    const int to_w = td_to.frame_scratch->width;
    const int to_h = td_to.frame_scratch->height;
    if (auto s = me::compose::frame_to_rgba8(
            td_to.frame_scratch.get(), to_rgba, err);
        s != ME_OK) {
        av_frame_unref(td_to.frame_scratch.get());
        return s;
    }
    av_frame_unref(td_to.frame_scratch.get());

    /* Enforce W×H for transition endpoint frames (phase-1). */
    if (to_w != W || to_h != H ||
        (from_valid && (from_w != W || from_h != H))) {
        if (err) {
            *err = "ComposeSink: cross-dissolve endpoint frame size "
                   "doesn't match output; transition rendering "
                   "requires W×H-matching source frames for phase-1 "
                   "(affine pre-composite during blend is a follow-up "
                   "of transition-with-transform backlog item)";
        }
        return ME_E_UNSUPPORTED;
    }

    const std::size_t bytes = static_cast<std::size_t>(W) * H * 4;
    if (track_rgba.size() != bytes) track_rgba.resize(bytes);

    if (from_valid) {
        me::compose::cross_dissolve(
            track_rgba.data(),
            from_rgba.data(), to_rgba.data(),
            W, H, static_cast<std::size_t>(W) * 4,
            fs.transition.t);
    } else {
        /* from exhausted — copy to into track_rgba unblended
         * (t effectively = 1). */
        std::memcpy(track_rgba.data(), to_rgba.data(), bytes);
    }

    out_src_w              = W;
    out_src_h              = H;
    out_transform_clip_idx = fs.transition_to_clip_idx;
    return ME_OK;
}

}  // namespace me::orchestrator
