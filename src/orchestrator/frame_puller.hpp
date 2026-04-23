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

#include "media_engine/types.h"

#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace me::orchestrator {

me_status_t pull_next_video_frame(
    AVFormatContext* demux,
    int              video_stream_idx,
    AVCodecContext*  dec,
    AVPacket*        pkt_scratch,
    AVFrame*         out_frame,
    std::string*     err);

}  // namespace me::orchestrator
