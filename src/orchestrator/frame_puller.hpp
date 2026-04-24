/*
 * frame_puller — one-frame-at-a-time video frame extractor.
 *
 * Given a (demux AVFormatContext, video stream index, opened decoder),
 * pulls the NEXT decoded video frame, driving the libav state machine
 * (av_read_frame → avcodec_send_packet → avcodec_receive_frame) until
 * a frame emerges or the source hits EOF.
 *
 * Scope: carved out of the `multi-track-compose-actual-composite` bullet
 * because the upcoming ComposeSink compose loop needs per-track
 * "advance this track one frame" primitives — and unit-testing one
 * function against a real fixture is cleaner than debugging the
 * full N-track compose path at once.
 *
 * Contract:
 *   - On success: `out_frame` holds a new decoded frame (ownership
 *     is the caller's — they `av_frame_unref` after use).
 *   - On EOF: returns ME_E_NOT_FOUND (no more frames; decoder has
 *     been fully flushed). Not an error.
 *   - On decode / I/O failure: returns ME_E_DECODE / ME_E_IO with
 *     err populated.
 *   - Reuses the caller-provided `pkt_scratch` (caller allocates once,
 *     frame_puller unrefs it each call so it can be reused).
 *
 * Non-video packets on other stream indices are skipped silently;
 * only video packets matching `video_stream_idx` are fed to the
 * decoder. Audio / subtitle streams interleaved in the same demux
 * are ignored by this helper (their processing is a separate
 * concern — the compose loop's audio path will read the same demux
 * through a parallel pull_next_audio_frame or pass the audio stream
 * to the existing reencode audio helpers).
 */
#pragma once

#include "io/ffmpeg_raii.hpp"
#include "media_engine/types.h"
#include "resource/codec_pool.hpp"

#include <memory>
#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace me::io { class DemuxContext; }

namespace me::orchestrator {

me_status_t pull_next_video_frame(
    AVFormatContext* demux,
    int              video_stream_idx,
    AVCodecContext*  dec,
    AVPacket*        pkt_scratch,
    AVFrame*         out_frame,
    std::string*     err);

/* Audio-side analog of pull_next_video_frame — drives the libav
 * state machine (av_read_frame → send_packet → receive_frame +
 * drain) to pull the next decoded audio AVFrame from a (demux,
 * decoder) pair. Same contract: ME_OK (frame filled, caller
 * av_frame_unref), ME_E_NOT_FOUND (clean EOF after drain),
 * ME_E_DECODE / ME_E_IO on errors, ME_E_INVALID_ARG on null args.
 *
 * Skips non-audio packets on other stream indices. Caller allocates
 * pkt_scratch + out_frame; this helper unrefs pkt_scratch per
 * iteration and populates out_frame on success. */
me_status_t pull_next_audio_frame(
    AVFormatContext* demux,
    int              audio_stream_idx,
    AVCodecContext*  dec,
    AVPacket*        pkt_scratch,
    AVFrame*         out_frame,
    std::string*     err);

/* Per-track decode state bundle: demux + video stream index +
 * opened video decoder + caller-reused scratch packet / frame for
 * pull_next_video_frame. `video_stream_idx < 0` means the demux has
 * no video stream (track has no video content — future audio-only
 * track case; the compose-loop caller can skip decoding for it).
 *
 * All owned members are RAII-managed: ~TrackDecoderState closes the
 * decoder, frees the scratch packet/frame, drops the shared_ptr<
 * DemuxContext> reference. Move-only (unique_ptr members). */
struct TrackDecoderState {
    std::shared_ptr<me::io::DemuxContext> demux;
    int                                    video_stream_idx = -1;
    me::resource::CodecPool::Ptr           dec;
    me::io::AvPacketPtr                    pkt_scratch;
    me::io::AvFramePtr                     frame_scratch;
};

/* Open a TrackDecoderState from a DemuxContext + CodecPool. On
 * success `out` is populated; on failure `out.demux` is reset and
 * err carries the diagnostic. If the demux has no video stream,
 * video_stream_idx stays -1 and dec is null but the other scratch
 * buffers are still allocated — caller treats this track as "no
 * video to contribute" per frame. */
me_status_t open_track_decoder(
    std::shared_ptr<me::io::DemuxContext> demux,
    me::resource::CodecPool&               pool,
    TrackDecoderState&                     out,
    std::string*                           err);

}  // namespace me::orchestrator
