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

bool spatial_identity(const me::Clip& c, int src_w, int src_h, int W, int H) {
    /* Identity = no Transform OR all-identity fields AND src dims match canvas.
     * If src dims differ from canvas, even "identity Transform" needs an
     * affine_blit to resize (technically a scale to W/src_w). */
    if (src_w != W || src_h != H) return false;
    if (!c.transform.has_value()) return true;
    const me::Transform& t = *c.transform;
    return t.translate_x  == 0.0 &&
           t.translate_y  == 0.0 &&
           t.scale_x      == 1.0 &&
           t.scale_y      == 1.0 &&
           t.rotation_deg == 0.0;
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

/* Apply clip.transform to src_rgba → dst_canvas (W×H×4). For
 * identity transform + src dims == W×H, this is a straight copy.
 * For any non-identity case or dim mismatch, affine_blit covers
 * both. dst_canvas is resized + zeroed-then-written by affine_blit;
 * we ensure it has the right size up front. */
void transform_to_canvas(const me::Clip& c,
                          const std::vector<std::uint8_t>& src_rgba,
                          int src_w, int src_h,
                          int W, int H,
                          std::vector<std::uint8_t>& dst_canvas) {
    const std::size_t bytes = static_cast<std::size_t>(W) * H * 4;
    if (dst_canvas.size() != bytes) dst_canvas.resize(bytes);

    if (spatial_identity(c, src_w, src_h, W, H)) {
        /* Fast path: direct copy — identity transform + canvas-sized src. */
        std::memcpy(dst_canvas.data(), src_rgba.data(), bytes);
        return;
    }

    /* Either non-identity Transform or src/canvas dim mismatch. Use
     * affine_blit: identity Transform + non-matching dims maps to
     * a scale-to-canvas (equivalent to clip-level stretch). */
    const me::Transform identity{};
    const me::Transform& t = c.transform.has_value() ? *c.transform : identity;
    const me::compose::AffineMatrix inv =
        me::compose::compose_inverse_affine(
            t.translate_x, t.translate_y,
            t.scale_x,     t.scale_y,
            t.rotation_deg,
            t.anchor_x,    t.anchor_y,
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
    const me::Clip&                 from_clip,
    const me::Clip&                 to_clip,
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
    /* from_valid == false iff from_pull == ME_E_NOT_FOUND. */

    /* Pull to_clip. Both endpoints drained → whole transition
     * contributes nothing at this T. */
    int to_w = 0, to_h = 0;
    bool to_valid = false;
    const me_status_t to_pull = decode_to_rgba(
        td_to, to_rgba, to_w, to_h, to_valid, err);
    if (to_pull == ME_E_NOT_FOUND) return ME_E_NOT_FOUND;
    if (to_pull != ME_OK) return to_pull;

    /* Pre-transform each endpoint to canvas size. This uniformly
     * applies per-clip Transform (or acts as copy-to-canvas for
     * identity + matching dims). */
    if (from_valid) {
        transform_to_canvas(from_clip, from_rgba, from_w, from_h, W, H, from_canvas);
    }
    transform_to_canvas(to_clip, to_rgba, to_w, to_h, W, H, to_canvas);

    const std::size_t bytes = static_cast<std::size_t>(W) * H * 4;
    if (track_rgba.size() != bytes) track_rgba.resize(bytes);

    if (from_valid) {
        me::compose::cross_dissolve(
            track_rgba.data(),
            from_canvas.data(), to_canvas.data(),
            W, H, static_cast<std::size_t>(W) * 4,
            fs.transition.t);
    } else {
        /* from exhausted — copy to into track_rgba unblended (t≈1). */
        std::memcpy(track_rgba.data(), to_canvas.data(), bytes);
    }

    out_src_w                    = W;
    out_src_h                    = H;
    out_transform_clip_idx       = fs.transition_to_clip_idx;
    out_spatial_already_applied  = true;   /* pre-transform always runs now */
    return ME_OK;
}

}  // namespace me::orchestrator
