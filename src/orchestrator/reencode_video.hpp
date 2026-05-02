/*
 * Video encoder helpers used by reencode_pipeline.cpp.
 *
 * Extracted so the pipeline orchestration TU stays focused on the
 * read→decode→dispatch→mux loop; the encoder-side setup and per-frame
 * encoding live here. These functions don't own any state beyond what
 * they're handed — they're free functions, not a class.
 */
#pragma once

#include "io/ffmpeg_raii.hpp"
#include "media_engine/codec_options.h"
#include "media_engine/types.h"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <string>

namespace me::orchestrator::detail {

/* Open a VideoToolbox video encoder sized / framed to match the
 * decoded stream. Dispatches on `video_codec`:
 *
 *   - `""` or `"h264"` → `h264_videotoolbox` + NV12 (8-bit Rec.709 SDR
 *     ship path; predates the M10 HEVC work).
 *   - `"hevc"`        → `hevc_videotoolbox` + P010 (10-bit, M10 HDR ship
 *     path per `encode-hevc-main10-hw`). Accepts SDR sources too —
 *     libswscale upsamples 8→10 in the existing convert path. When
 *     `hevc_videotoolbox` is missing (Linux / Windows hosts), the
 *     UNSUPPORTED diagnostic explicitly names `"hevc-sw"` so callers
 *     know the LGPL-clean SW fallback opt-in.
 *   - `"hevc-sw"`     → preflight for the Kvazaar SW HEVC fallback
 *     (M10 §3 LGPL-clean, BSD-3 Kvazaar). Validates ME_HAS_KVAZAAR +
 *     1080p ceiling + multiple-of-8 alignment to mirror
 *     `KvazaarHevcEncoder::create`'s checks. The encode-loop wiring
 *     (Annex-B chunks → AVPacket → mux) is tracked by BACKLOG bullet
 *     `encode-hevc-sw-encode-loop-impl`; this preflight returns
 *     ME_E_UNSUPPORTED with that bullet name in the diagnostic so
 *     callers see a stable error point.
 *
 * Color tags (range / primaries / TRC / matrix) propagate from the
 * decoder so HDR sources tagged BT.2020 + PQ surface as HDR10
 * end-to-end. VideoToolbox emits the matching ST 2086 / CTA-861.3
 * SEI from those tags automatically; explicit MasteringDisplay /
 * ContentLight side-data attachment for custom values lands with a
 * future cycle.
 *
 * Callers stage an sws_scale into the encoder's pix_fmt when the
 * decoded pix_fmt differs (the existing `shared.venc_pix` plumb).
 * `global_header` must be true when the output container has
 * AVFMT_GLOBALHEADER (MP4/MOV) — must be set before avcodec_open2.
 *
 * Unknown / unsupported `video_codec_enum` → ME_E_UNSUPPORTED.
 * The `video_codec` string is the diagnostic source for the error
 * message (the resolver coerces unknown strings to NONE; the
 * original string preserves what the host actually wrote). */
me_status_t open_video_encoder(me::resource::CodecPool&      pool,
                               const AVCodecContext*         dec,
                               AVRational                    stream_time_base,
                               int64_t                       bitrate_bps,
                               bool                          global_header,
                               me_video_codec_t              video_codec_enum,
                               const std::string&            video_codec,
                               me::resource::CodecPool::Ptr& out_enc,
                               AVPixelFormat&                out_target_pix,
                               std::string*                  err);

/* Encode one decoded video frame (or nullptr for flush). Scales into NV12
 * when `sws` is provided. Drains the encoder and writes produced packets
 * via av_interleaved_write_frame. */
me_status_t encode_video_frame(AVFrame*         in_frame,
                               AVCodecContext*  enc,
                               SwsContext*      sws,
                               AVFrame*         scratch_nv12,
                               AVFormatContext* ofmt,
                               int              out_stream_idx,
                               AVRational       in_stream_tb,
                               std::string*     err);

/* Compute an output-stream PTS that preserves source-stream timing.
 *
 * Background: the old reencode path overwrote every frame's PTS with
 * a CFR counter (`next_video_pts += delta`), which flattens VFR input
 * to CFR output — accumulating A/V drift against the audio stream
 * (which stays anchored to source sample-count timing). This helper
 * computes the "right" output PTS: anchor at the first frame's
 * source PTS, keep the inter-frame intervals intact, add a segment-
 * base offset for multi-segment concatenation.
 *
 * Inputs:
 *   src_pts              — current frame's PTS in source stream tb.
 *   first_src_pts        — PTS of this segment's first video frame
 *                          (in source stream tb). Anchors output to 0
 *                          at segment start.
 *   src_tb               — source stream time_base (e.g. {1, 30}).
 *   out_tb               — encoder time_base.
 *   segment_base_out_pts — cumulative output PTS at segment start
 *                          (prior segments' total duration in out_tb).
 *
 * Returns: `segment_base_out_pts + av_rescale_q(src_pts - first_src_pts,
 * src_tb, out_tb)`.
 *
 * Pure math; no libav mutation — tests drive it with synthetic ints. */
int64_t remap_source_pts_to_output(int64_t     src_pts,
                                    int64_t     first_src_pts,
                                    AVRational  src_tb,
                                    AVRational  out_tb,
                                    int64_t     segment_base_out_pts);

}  // namespace me::orchestrator::detail
