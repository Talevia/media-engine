/*
 * me::orchestrator::run_compose_video_frame_loop — extracted
 * per-output-frame compose loop for ComposeSink::process().
 *
 * Scope-B slice of debt-split-compose-sink-process. compose_sink.cpp
 * hit 600 lines after the gpu-backend plumbing landed in 15f7784,
 * with `ComposeSink::process` spanning 84→485 — a 400-line method.
 * Two prior extractions (compose_audio {setup_compose_audio_mixer,
 * drain_compose_audio} in 700ec31, compose_transition_step in
 * 4cd106f) used the same sibling-TU pattern; this cycle continues
 * with the biggest remaining blob, the ~210-line per-frame compose
 * loop.
 *
 * Contract:
 *   - One call runs every output frame from `fi=0` to
 *     `total_frames - 1`, where total_frames is derived from
 *     `ctx.tl.duration * ctx.tl.frame_rate` inside.
 *   - Per frame: clears dst_rgba to opaque black → walks video
 *     tracks bottom→top, calling `frame_source_at` + either the
 *     SingleClip or Transition path → alpha_over composites each
 *     track's output → converts dst_rgba to YUV and encodes.
 *   - Honors `shared.cancel` — returns `ME_E_CANCELLED` early if
 *     the flag becomes set between frames.
 *   - Updates `shared.next_video_pts` as frames are submitted.
 *   - Fires `shared.on_ratio` progress callback per frame when set.
 *
 * Ownership: all buffers + target_yuv are references / raw pointers
 * borrowed from the caller; the loop does not allocate or free.
 */
#pragma once

#include "orchestrator/frame_puller.hpp"
#include "orchestrator/reencode_segment.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
struct AVFrame;
}

#include <cstdint>
#include <string>
#include <vector>

namespace me::orchestrator {

struct ComposeVideoLoopCtx {
    const me::Timeline&                tl;
    std::vector<TrackDecoderState>&    clip_decoders;
    detail::SharedEncState&            shared;

    int                                W;
    int                                H;

    /* RGBA working buffers owned by the caller (one set, reused
     * across frames). Only `dst_rgba` / `track_rgba` /
     * `track_rgba_xform` are touched in the SingleClip path;
     * transition path additionally populates the four
     * `{from,to}_{rgba,canvas}` buffers. */
    std::vector<std::uint8_t>&         dst_rgba;
    std::vector<std::uint8_t>&         track_rgba;
    std::vector<std::uint8_t>&         track_rgba_xform;
    std::vector<std::uint8_t>&         from_rgba;
    std::vector<std::uint8_t>&         to_rgba;
    std::vector<std::uint8_t>&         from_canvas;
    std::vector<std::uint8_t>&         to_canvas;

    /* Encoder target frame — allocated + sized by the caller with
     * format = shared.venc_pix. Loop writes `pts` / `pkt_dts` per
     * iteration and passes to `detail::encode_video_frame`. */
    ::AVFrame*                         target_yuv;
};

me_status_t run_compose_video_frame_loop(
    ComposeVideoLoopCtx& ctx,
    std::string*         err);

}  // namespace me::orchestrator
