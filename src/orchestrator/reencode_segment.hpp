/*
 * Per-segment re-encode driver — extracted from reencode_pipeline.cpp.
 *
 * `reencode_pipeline.cpp` used to host three concerns in one TU: (1) the
 * top-level `reencode_mux` orchestrator that opens the mux + shared
 * encoders + writes the trailer, (2) the `SharedEncState` struct that
 * carries encoder/mux handles + running PTS counters across segments,
 * (3) the `process_segment` worker that opens a per-clip decoder,
 * seeks, decodes, feeds the shared encoder, and flushes the decoder at
 * end-of-clip. Keeping all three together pushed the file to 523 lines
 * post the `debt-split-reencode-pipeline-audio-fifo` cycle.
 *
 * This header pulls (2) and (3) — plus the `open_decoder` helper both
 * the pipeline and the segment loop need, and the `total_output_us`
 * helper the pipeline uses to seed progress — into `reencode_segment.cpp`,
 * leaving `reencode_pipeline.cpp` to focus on orchestration.
 */
#pragma once

#include "color/pipeline.hpp"
#include "io/demux_context.hpp"
#include "media_engine/types.h"
#include "orchestrator/reencode_pipeline.hpp"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace me::orchestrator::detail {

/* Shared encoder / muxer state threaded through per-segment processing.
 * `reencode_mux` owns the AVCodecContext / AVFormatContext handles and
 * writes the running PTS counters; `process_segment` reads encoder
 * params, stamps frames with `next_video_pts` / `next_audio_pts`, and
 * mutates those counters as frames flow through.
 *
 * Cross-segment continuity: `next_*_pts` accumulate monotonically
 * across the entire segment list so the encoder sees one continuous
 * timeline (no reset on segment boundaries — that would produce a
 * broken H.264 bitstream). */
struct SharedEncState {
    AVFormatContext* ofmt          = nullptr;
    AVCodecContext*  venc          = nullptr;
    AVCodecContext*  aenc          = nullptr;
    int              out_vidx      = -1;
    int              out_aidx      = -1;
    AVPixelFormat    venc_pix      = AV_PIX_FMT_NONE;
    AVAudioFifo*     afifo         = nullptr;

    /* Fixed output video frame duration in venc->time_base — derived
     * once from segment[0]'s frame rate. Restamping each decoded frame
     * with a running counter incremented by this delta produces CFR
     * output even from VFR inputs, which is standard re-encode
     * behavior. */
    int64_t          video_pts_delta  = 0;
    int64_t          next_video_pts   = 0;
    /* Running output-tb PTS for audio. The FIFO drain reads exactly
     * aenc->frame_size samples per encoded frame and stamps them with
     * this monotonically-incrementing counter, so segment boundaries
     * are transparent to the audio encoder — they're just more samples
     * flowing through the FIFO. */
    int64_t          next_audio_pts   = 0;

    /* Expected source params, populated from segment[0]'s decoders.
     * `process_segment` checks every subsequent segment's decoder
     * against these and fails fast on mismatch — re-encoder can't
     * honour a mid-stream resolution / sample-rate change without a
     * full encoder re-open, which is explicitly out of phase-1 scope. */
    int              v_width   = 0;
    int              v_height  = 0;
    AVPixelFormat    v_pix     = AV_PIX_FMT_NONE;
    int              a_sr      = 0;
    AVSampleFormat   a_fmt     = AV_SAMPLE_FMT_NONE;
    int              a_chans   = 0;

    const std::atomic<bool>*        cancel  = nullptr;
    std::function<void(float)>      on_ratio;
    int64_t                         total_us  = 0;

    /* First consumer of `me::color::make_pipeline()`, per the
     * `ocio-pipeline-factory` + `ocio-pipeline-wire-first-consumer`
     * cycles. Today this is always an `IdentityPipeline` (no-op apply)
     * so wiring in this seat is safe on the determinism path; when
     * `ME_WITH_OCIO` flips on and `make_pipeline()` starts returning
     * `OcioPipeline`, the per-frame apply call in `process_segment`'s
     * `push_video_frame` activates real color conversion without the
     * orchestration TU having to change. Ownership is `unique_ptr`
     * so per-segment decoders / encoders never see the Pipeline — it's
     * a SharedEncState concern, like the muxer / FIFO. */
    std::unique_ptr<me::color::Pipeline> color_pipeline;
};

/* Open a decoder for `in_stream` via `pool`. Used by both `reencode_mux`
 * (param-sniffing segment[0] before opening the shared encoder) and
 * `process_segment` (per-clip decoders during the main loop). */
me_status_t open_decoder(me::resource::CodecPool&      pool,
                         AVStream*                     in_stream,
                         me::resource::CodecPool::Ptr& out,
                         std::string*                  err);

/* Sum each segment's source_duration (falling back to demux duration
 * minus source_start) in AV_TIME_BASE_Q. Used to seed
 * `SharedEncState::total_us` for progress-ratio reporting. */
int64_t total_output_us(const std::vector<ReencodeSegment>& segs);

/* Process one segment into the shared encoder: open its decoders, seek
 * to source_start, loop read→decode→encode, flush the per-segment
 * decoders into the shared encoder. The shared encoder is NOT flushed
 * here — that happens once at the end of the full segment list inside
 * `reencode_mux`. */
me_status_t process_segment(const ReencodeSegment&      seg,
                            me::resource::CodecPool&    pool,
                            SharedEncState&             shared,
                            std::size_t                 seg_idx,
                            std::string*                err);

}  // namespace me::orchestrator::detail
