/*
 * me::audio::AudioTrackFeed — per-track audio pipeline: demux →
 * decode → resample to target format → per-clip gain.
 *
 * Scope-A slice of `audio-mix-scheduler-wire` sub-scope (1). One
 * feed represents one audio track's contribution to the eventual
 * AudioMixer: it binds a demux + audio stream decoder together with
 * the mix target params (rate / sample_fmt / channel_layout) and a
 * linear gain. `pull_next_processed_audio_frame` yields the next
 * target-formatted, gain-applied AVFrame on demand; the follow-up
 * mixer cycle wraps N of these, sums their sample planes, and runs
 * peak_limiter before emitting to the encoder.
 *
 * Deliberate separation from `TrackDecoderState` (orchestrator/
 * frame_puller.hpp, video-side): the video track-state only holds
 * demux + decoder + scratch because its downstream consumer
 * (ComposeSink) maintains its own RGBA working buffers and picks a
 * target resolution per clip-Transform. Audio's downstream mixer
 * needs the resample target binding + gain binding up front —
 * bundling them in the feed avoids re-configuring libswresample per
 * pull and lets the open step early-fail if the source format is
 * incompatible.
 *
 * Ownership / lifetime:
 *   - `demux` is shared_ptr — same convention as TrackDecoderState.
 *     Loader's per-clip DemuxContext lives as long as any feed /
 *     decoder reads from it.
 *   - `dec`, `pkt_scratch`, `frame_scratch` are pool/RAII-owned;
 *     feed destructor releases them.
 *   - `target_ch_layout` is heap-managed by libav; feed destructor
 *     calls `av_channel_layout_uninit`.
 *   - Move-only (unique_ptr members + AVChannelLayout that requires
 *     explicit uninit).
 */
#pragma once

#include "io/ffmpeg_raii.hpp"
#include "media_engine/types.h"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <memory>
#include <string>

struct AVFrame;

namespace me::io { class DemuxContext; }

namespace me::audio {

struct AudioTrackFeed {
    std::shared_ptr<me::io::DemuxContext> demux;
    int                                    audio_stream_idx = -1;
    me::resource::CodecPool::Ptr           dec;
    me::io::AvPacketPtr                    pkt_scratch;
    me::io::AvFramePtr                     frame_scratch;

    /* Target mix format — set at open time. All pulls return frames
     * at these params. FLTP is the canonical mixer working format. */
    int             target_rate     = 0;
    AVSampleFormat  target_fmt      = AV_SAMPLE_FMT_NONE;
    AVChannelLayout target_ch_layout{};   /* owned; uninit in dtor */

    /* Per-clip gain applied to each pulled frame's samples. 1.0 =
     * unity passthrough; 0.0 = silence; typically pre-computed from
     * gainDb via `me::audio::db_to_linear`. */
    float gain_linear = 1.0f;

    /* Sticky EOF flag — once the underlying decoder returns
     * NOT_FOUND, subsequent pulls short-circuit without hitting
     * libav. Caller checks `feed.eof` or looks for NOT_FOUND return. */
    bool  eof         = false;

    AudioTrackFeed() = default;
    ~AudioTrackFeed();
    AudioTrackFeed(const AudioTrackFeed&)            = delete;
    AudioTrackFeed& operator=(const AudioTrackFeed&) = delete;
    AudioTrackFeed(AudioTrackFeed&&) noexcept;
    AudioTrackFeed& operator=(AudioTrackFeed&&) noexcept;
};

/* Open the best audio stream on `demux` and produce a configured
 * feed. `target_rate` / `target_fmt` / `target_ch_layout` are the
 * downstream mixer's working format — all pulls produce AVFrames at
 * these params (libswresample handles conversion per call). Returns:
 *   - ME_OK on success
 *   - ME_E_NOT_FOUND if the demux has no audio stream
 *   - ME_E_INVALID_ARG on null demux / zero target_rate / NONE target_fmt
 *   - ME_E_UNSUPPORTED if no decoder registered for the stream's codec
 *   - ME_E_OUT_OF_MEMORY / ME_E_INTERNAL on libav setup failure */
me_status_t open_audio_track_feed(
    std::shared_ptr<me::io::DemuxContext> demux,
    me::resource::CodecPool&               pool,
    int                                    target_rate,
    AVSampleFormat                         target_fmt,
    const AVChannelLayout&                 target_ch_layout,
    float                                  gain_linear,
    AudioTrackFeed&                        out,
    std::string*                           err);

/* Pull the next decoded audio frame from the feed's decoder,
 * resample to the target format, and apply gain_linear scaling.
 *
 * On success (ME_OK): `*out_frame` is a freshly-allocated AVFrame
 * at (target_rate, target_fmt, target_ch_layout) with samples
 * gain-scaled. Caller owns — free via `av_frame_free(&f)`.
 *
 * On EOF (ME_E_NOT_FOUND): `*out_frame == nullptr`, `feed.eof = true`.
 *
 * On error (ME_E_DECODE / ME_E_IO / ME_E_INTERNAL / ME_E_OUT_OF_MEMORY):
 * `*out_frame == nullptr`, err populated. `feed.eof` unchanged —
 * caller decides whether to retry or abandon.
 *
 * Gain apply scope: only AV_SAMPLE_FMT_FLTP (planar float) is
 * gain-adjusted in-place. For other target formats the frame returns
 * unscaled — the M2 mixer mandates FLTP working format so this
 * doesn't block phase-1 usage. */
me_status_t pull_next_processed_audio_frame(
    AudioTrackFeed& feed,
    AVFrame**       out_frame,
    std::string*    err);

}  // namespace me::audio
