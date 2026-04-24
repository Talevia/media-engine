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

    /* Set by compose_transition_step after pulling this decoder as
     * the to_clip endpoint of a cross-dissolve window. The transition
     * path consumes `duration × fps` frames from to_clip during the
     * window (sequential pull per emitted frame), leaving the decoder
     * `duration/2 × fps` frames ahead of the schema-expected source
     * position at window_end. The next SingleClip pull for this clip
     * checks the flag, performs a frame-accurate seek to the
     * schema-aligned source_time, then clears the flag and proceeds
     * with sequential pulls. Phase-1 convention: "to_clip plays from
     * frame 0 during window; realign at window end". See
     * `transition-to-clip-source-time-align` decision. */
    bool                                   used_as_to_in_transition = false;
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

/* Frame-accurate seek on an open TrackDecoderState:
 *
 *   1. `avformat_seek_file(BACKWARD)` to the keyframe at or before
 *      the target source_time (converted to AV_TIME_BASE µs).
 *   2. `avcodec_flush_buffers` to drop stale decoder state.
 *   3. Decode forward, discarding frames, until the first frame
 *      whose pts in stream time-base is at or after target.
 *
 * On ME_OK: `td.frame_scratch` holds the target frame. Caller
 * treats this as if `pull_next_video_frame` just returned — run
 * `frame_to_rgba8` + `av_frame_unref(td.frame_scratch.get())`
 * as usual, then subsequent pulls go back through
 * `pull_next_video_frame`.
 *
 * Returns:
 *   - ME_OK: frame_scratch populated with target frame.
 *   - ME_E_NOT_FOUND: seek OK but decoder drained before reaching
 *     target (asset shorter than target). `frame_scratch` unref'd.
 *   - ME_E_IO: av_seek_file failure (when target > 1s; small-offset
 *     seek failures fall through to "decode from start").
 *   - ME_E_DECODE: send/receive failure while decoding forward.
 *   - ME_E_INVALID_ARG: null/unopened td.
 *
 * Intended caller: ComposeSink's SingleClip branch when it detects
 * a to_clip decoder coming out of a cross-dissolve window (see
 * `used_as_to_in_transition` flag on TrackDecoderState). */
me_status_t seek_track_decoder_frame_accurate_to(
    TrackDecoderState& td,
    me_rational_t      target_source_time,
    std::string*       err);

}  // namespace me::orchestrator
