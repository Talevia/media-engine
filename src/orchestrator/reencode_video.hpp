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
#include "media_engine/types.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <string>

namespace me::orchestrator::detail {

/* Open `h264_videotoolbox` sized / framed to match the decoded stream.
 * NV12 is the native VideoToolbox surface format; callers stage an
 * sws_scale into NV12 when the decoded pix_fmt differs. `global_header`
 * must be true when the output container has AVFMT_GLOBALHEADER (MP4/MOV)
 * — must be set before avcodec_open2. */
me_status_t open_video_encoder(const AVCodecContext*      dec,
                               AVRational                 stream_time_base,
                               int64_t                    bitrate_bps,
                               bool                       global_header,
                               me::io::AvCodecContextPtr& out_enc,
                               AVPixelFormat&             out_target_pix,
                               std::string*               err);

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

}  // namespace me::orchestrator::detail
