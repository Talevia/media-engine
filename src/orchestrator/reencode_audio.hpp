/*
 * Audio encoder helpers used by reencode_pipeline.cpp.
 *
 * Extracted from the orchestration TU so the open + per-frame encode
 * concerns sit next to each other. The FIFO-buffering / drain loop that
 * handles AAC's fixed frame_size constraint lives in reencode_pipeline.cpp
 * alongside the main decode dispatch — splitting that out would require
 * a multi-field state class that doesn't yet have a second consumer.
 */
#pragma once

#include "io/ffmpeg_raii.hpp"
#include "media_engine/types.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdint>
#include <string>

namespace me::orchestrator::detail {

/* Open libavcodec's built-in AAC encoder (NOT `aac_at`) so the encode
 * stays pure-LGPL. Clamps off-grid sample rates to 48 kHz (the MPEG-4
 * AAC table is fixed). Must be called before output_stream time_base
 * configuration. */
me_status_t open_audio_encoder(const AVCodecContext*      dec,
                               int64_t                    bitrate_bps,
                               bool                       global_header,
                               me::io::AvCodecContextPtr& out_enc,
                               std::string*               err);

/* Encode one audio frame (or nullptr for flush). Drains the encoder and
 * writes produced packets via av_interleaved_write_frame. Unlike video,
 * no pixfmt conversion needed here — caller ensures `in_frame` is already
 * at `enc->sample_fmt`. */
me_status_t encode_audio_frame(AVFrame*         in_frame,
                               AVCodecContext*  enc,
                               AVFormatContext* ofmt,
                               int              out_stream_idx,
                               std::string*     err);

}  // namespace me::orchestrator::detail
