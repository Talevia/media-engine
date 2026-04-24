#include "orchestrator/compose_transition_step.hpp"

#include "compose/affine_blit.hpp"
#include "compose/cross_dissolve.hpp"
#include "compose/frame_convert.hpp"
#include "io/demux_context.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <cstring>

namespace me::orchestrator {

namespace {

/* Identity iff transform is absent OR all spatial fields at default
 * AND src dims match canvas. Dim-mismatch alone triggers affine_blit
 * (scale-to-canvas). */
bool spatial_identity_for(const me::TransformEvaluated& tr,
                           bool                          has_transform,
                           int src_w, int src_h, int W, int H) {
    if (src_w != W || src_h != H) return false;
    if (!has_transform) return true;
    return tr.spatial_identity();
}

me_status_t decode_to_rgba(TrackDecoderState& td,
                           std::vector<std::uint8_t>& out_rgba,
                           int& out_w, int& out_h,
                           bool& out_valid,
                           std::string* err) {
    const me_status_t pull = pull_next_video_frame(
        td.demux->fmt, td.video_stream_idx, td.dec.get(),
        td.pkt_scratch.get(), td.frame_scratch.get(), err);
    if (pull == ME_E_NOT_FOUND) {
        out_valid = false;
        return ME_E_NOT_FOUND;
    }
    if (pull != ME_OK) {
        out_valid = false;
        return pull;
    }
    out_w = td.frame_scratch->width;
    out_h = td.frame_scratch->height;
    const me_status_t s = me::compose::frame_to_rgba8(
        td.frame_scratch.get(), out_rgba, err);
    av_frame_unref(td.frame_scratch.get());
    if (s != ME_OK) {
        out_valid = false;
        return s;
    }
    out_valid = true;
    return ME_OK;
}

/* Apply (already-evaluated) transform to src_rgba → dst_canvas
 * (W×H×4). Identity + matching dims = direct memcpy; otherwise
 * affine_blit handles everything (including pure dim scale). */
void transform_to_canvas(const me::TransformEvaluated& tr,
                          bool                          has_transform,
                          const std::vector<std::uint8_t>& src_rgba,
                          int src_w, int src_h,
                          int W, int H,
                          std::vector<std::uint8_t>& dst_canvas) {
    const std::size_t bytes = static_cast<std::size_t>(W) * H * 4;
    if (dst_canvas.size() != bytes) dst_canvas.resize(bytes);

    if (spatial_identity_for(tr, has_transform, src_w, src_h, W, H)) {
        std::memcpy(dst_canvas.data(), src_rgba.data(), bytes);
        return;
    }

    const me::compose::AffineMatrix inv =
        me::compose::compose_inverse_affine(
            tr.translate_x, tr.translate_y,
            tr.scale_x,     tr.scale_y,
            tr.rotation_deg,
            tr.anchor_x,    tr.anchor_y,
            src_w, src_h);
    me::compose::affine_blit(
        dst_canvas.data(), W, H, static_cast<std::size_t>(W) * 4,
        src_rgba.data(),    src_w, src_h,
        static_cast<std::size_t>(src_w) * 4,
        inv);
}

}  // namespace

me_status_t compose_transition_step(
    const me::compose::FrameSource& fs,
    const me::TransformEvaluated&   from_tr,
    bool                            from_has_transform,
    const me::TransformEvaluated&   to_tr,
    bool                            to_has_transform,
    TrackDecoderState&              td_from,
    TrackDecoderState&              td_to,
    int                             W,
    int                             H,
    std::vector<std::uint8_t>&      track_rgba,
    std::vector<std::uint8_t>&      from_rgba,
    std::vector<std::uint8_t>&      to_rgba,
    std::vector<std::uint8_t>&      from_canvas,
    std::vector<std::uint8_t>&      to_canvas,
    int&                            out_src_w,
    int&                            out_src_h,
    std::size_t&                    out_transform_clip_idx,
    bool&                           out_spatial_already_applied,
    std::string*                    err) {

    /* Pull from_clip. NOT_FOUND → degrade to to-only (no cached
     * from frame yet; real handles/seek is a future bullet). */
    int from_w = 0, from_h = 0;
    bool from_valid = false;
    const me_status_t from_pull = decode_to_rgba(
        td_from, from_rgba, from_w, from_h, from_valid, err);
    if (from_pull != ME_OK && from_pull != ME_E_NOT_FOUND) return from_pull;

    /* Pull to_clip. Both endpoints drained → transition contributes
     * nothing at this T. */
    int to_w = 0, to_h = 0;
    bool to_valid = false;
    const me_status_t to_pull = decode_to_rgba(
        td_to, to_rgba, to_w, to_h, to_valid, err);
    if (to_pull == ME_E_NOT_FOUND) return ME_E_NOT_FOUND;
    if (to_pull != ME_OK) return to_pull;

    if (from_valid) {
        transform_to_canvas(from_tr, from_has_transform,
                             from_rgba, from_w, from_h, W, H, from_canvas);
    }
    transform_to_canvas(to_tr, to_has_transform,
                         to_rgba, to_w, to_h, W, H, to_canvas);

    const std::size_t bytes = static_cast<std::size_t>(W) * H * 4;
    if (track_rgba.size() != bytes) track_rgba.resize(bytes);

    if (from_valid) {
        me::compose::cross_dissolve(
            track_rgba.data(),
            from_canvas.data(), to_canvas.data(),
            W, H, static_cast<std::size_t>(W) * 4,
            fs.transition.t);
    } else {
        std::memcpy(track_rgba.data(), to_canvas.data(), bytes);
    }

    out_src_w                    = W;
    out_src_h                    = H;
    out_transform_clip_idx       = fs.transition_to_clip_idx;
    out_spatial_already_applied  = true;
    return ME_OK;
}

}  // namespace me::orchestrator
