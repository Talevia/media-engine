/*
 * Audio encoder helpers used by reencode_pipeline.cpp.
 *
 * The three concerns grouped here are:
 *   1. Open the AAC encoder matched to a decoder's params (`open_audio_encoder`).
 *   2. Push a single decoded audio frame into the encoder, possibly through
 *      swr + a FIFO to match the encoder's fixed frame_size (`feed_audio_frame`).
 *   3. Drain the FIFO in encoder-sized chunks (`drain_audio_fifo`).
 *
 * All four helpers live together so the orchestration TU
 * (`reencode_pipeline.cpp`) stays focused on the main decode→dispatch→mux
 * loop and doesn't carry per-sample swr / FIFO plumbing inline.
 */
#pragma once

#include "io/ffmpeg_raii.hpp"
#include "media_engine/types.h"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

#include <cstdint>
#include <string>

namespace me::orchestrator::detail {

/* Open libavcodec's built-in AAC encoder (NOT `aac_at`) so the encode
 * stays pure-LGPL. Clamps off-grid sample rates to 48 kHz (the MPEG-4
 * AAC table is fixed). Must be called before output_stream time_base
 * configuration. */
me_status_t open_audio_encoder(me::resource::CodecPool&      pool,
                               const AVCodecContext*         dec,
                               int64_t                       bitrate_bps,
                               bool                          global_header,
                               me::resource::CodecPool::Ptr& out_enc,
                               std::string*                  err);

/* Encode one audio frame (or nullptr for flush). Drains the encoder and
 * writes produced packets via av_interleaved_write_frame. Unlike video,
 * no pixfmt conversion needed here — caller ensures `in_frame` is already
 * at `enc->sample_fmt`. */
me_status_t encode_audio_frame(AVFrame*         in_frame,
                               AVCodecContext*  enc,
                               AVFormatContext* ofmt,
                               int              out_stream_idx,
                               std::string*     err);

/* Drain `afifo` into `aenc` in encoder-sized chunks. Each drained chunk
 * gets stamped with `*next_pts_in_enc_tb`, which is then incremented by
 * the chunk's sample count — this makes segment boundaries transparent
 * to the encoder (reencode_multi_clip reuses the FIFO + the counter
 * across N segments). When `flush=true` the final partial chunk is
 * allowed; otherwise the function returns ME_OK as soon as the FIFO has
 * less than `aenc->frame_size` samples. */
me_status_t drain_audio_fifo(AVAudioFifo*     afifo,
                             AVCodecContext*  aenc,
                             AVFormatContext* ofmt,
                             int              out_stream_idx,
                             int64_t*         next_pts_in_enc_tb,
                             bool             flush,
                             std::string*     err);

/* Convert a single decoded audio frame through `swr` into `afifo`, then
 * drain any encoder-sized chunks that accumulated. Passing `nullptr` for
 * `in_frame` signals "no more input samples" — swr's residual delay is
 * flushed into the FIFO but the FIFO itself is not force-drained (caller
 * uses `drain_audio_fifo(..., flush=true)` at end-of-stream for that). */
me_status_t feed_audio_frame(AVFrame*         in_frame,
                             SwrContext*      swr,
                             int              src_sample_rate,
                             AVAudioFifo*     afifo,
                             AVCodecContext*  aenc,
                             AVFormatContext* ofmt,
                             int              out_stream_idx,
                             int64_t*         next_pts_in_enc_tb,
                             std::string*     err);

}  // namespace me::orchestrator::detail
