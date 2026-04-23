/*
 * encoder_mux_setup — factored-out h264/aac encoder + mux bootstrap.
 *
 * The same ~100 lines of plumbing (open sample decoders for param
 * inference → open MuxContext → open_video_encoder + open_audio_encoder
 * → populate SharedEncState → av_audio_fifo_alloc) is needed by
 * `reencode_mux` (single-track concat), the upcoming ComposeSink
 * frame loop (multi-track compose), the cross-dissolve transition
 * sink, and the audio-mix scheduler. Extracted here so each of those
 * consumers is just a thin setup + per-frame-loop body, not a copy-
 * paste of the bootstrap.
 *
 * Ownership model: caller declares `mux` / `venc` / `aenc` as their
 * own local `unique_ptr` / `CodecCtxPtr`; this function populates
 * them. `shared` is a SharedEncState value on the caller's stack and
 * is filled in-place. The caller still owns the audio FIFO guard
 * (wraps `shared.afifo` in its own inline RAII), matching the existing
 * reencode_mux pattern.
 *
 * No behavior change vs the in-line setup in reencode_pipeline.cpp —
 * this is a pure refactor extracted verbatim. All 22 existing ctest
 * suites continue to pass without modification.
 */
#pragma once

#include "orchestrator/reencode_pipeline.hpp"  /* ReencodeOptions */
#include "orchestrator/reencode_segment.hpp"   /* detail::SharedEncState */
#include "resource/codec_pool.hpp"
#include "io/mux_context.hpp"

#include <memory>
#include <string>

struct AVFormatContext;

namespace me::orchestrator {

/* Bootstrap h264/aac encoder + output mux from a sample demux used
 * for parameter inference (dimensions, frame rate, sample rate, etc).
 * On success:
 *   - out_mux is opened (AVFormatContext allocated; AVIO NOT yet opened,
 *     and write_header NOT yet called — caller does those at their
 *     preferred sequence point).
 *   - out_venc / out_aenc own the opened encoder contexts (nullable if
 *     the sample demux had no corresponding stream).
 *   - out_shared.{venc, aenc, ofmt, out_vidx, out_aidx, v_*, a_*,
 *     venc_pix, video_pts_delta, afifo, cancel, on_ratio, total_us,
 *     color_pipeline, target_color_space} are populated.
 *   - out_shared.afifo is raw-owned — caller must `av_audio_fifo_free`
 *     on scope exit (mirror reencode_mux's inline FifoGuard).
 *
 * On failure: out_* pointers are empty and err carries the diagnostic.
 *
 * Non-null preconditions: `opts.pool`, `sample_demux` all required. */
me_status_t setup_h264_aac_encoder_mux(
    const ReencodeOptions&                 opts,
    AVFormatContext*                       sample_demux,
    std::unique_ptr<me::io::MuxContext>&   out_mux,
    me::resource::CodecPool::Ptr&          out_venc,
    me::resource::CodecPool::Ptr&          out_aenc,
    detail::SharedEncState&                out_shared,
    std::string*                           err);

}  // namespace me::orchestrator
