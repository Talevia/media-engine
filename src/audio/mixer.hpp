/*
 * me::audio::AudioMixer — N-track audio mixer.
 *
 * Scope-A slice of `audio-mix-scheduler-wire` sub-scope (1). Takes
 * N AudioTrackFeed instances (each pre-configured to the same
 * target rate / sample_fmt / channel_layout) and emits fixed-size
 * mixed AVFrames of `frame_size` samples. Internally maintains a
 * per-track sample FIFO: pulls AVFrames from each feed as needed
 * to keep enough samples buffered for the next output chunk,
 * sums cross-track via `mix_samples`, optionally peak-limits,
 * and serves out-chunks aligned to the AAC encoder's natural
 * frame size (1024).
 *
 * Phase-1 constraints (matches AudioTrackFeed + mix_samples):
 *   - Target format must be AV_SAMPLE_FMT_FLTP (planar float).
 *   - All feeds must share the same target (rate, fmt, ch_layout)
 *     — enforced at `add_track` time; mixer does not re-resample.
 *   - `mix_samples` applies gain_linear=1.0 per input: per-clip
 *     gain was already applied by AudioTrackFeed during pull.
 *     Mixer's responsibility is purely sum + limit.
 *
 * Ownership:
 *   - Mixer owns N AudioTrackFeed instances (moved in via
 *     `add_track`).
 *   - `pull_next_mixed_frame` returns a freshly-allocated AVFrame
 *     the caller owns (av_frame_free).
 *
 * Not wired into any sink this cycle. Follow-up
 * `audio-mix-scheduler-wire` sub-scopes: H264AacSink refactor,
 * Exporter audio track gate flip, 2-track e2e determinism test.
 */
#pragma once

#include "audio/track_feed.hpp"
#include "media_engine/types.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <memory>
#include <string>
#include <vector>

namespace me { struct Timeline; }
namespace me::io { class DemuxContext; }
namespace me::resource { class CodecPool; }

struct AVFrame;

namespace me::audio {

struct AudioMixerConfig {
    int             target_rate     = 48000;
    AVSampleFormat  target_fmt      = AV_SAMPLE_FMT_FLTP;
    AVChannelLayout target_ch_layout{};

    /* Samples per emitted mixed AVFrame. 1024 is the AAC LC
     * canonical frame size; caller sizes this to whatever the
     * downstream encoder requires. Must be > 0. */
    int             frame_size      = 1024;

    /* Peak limiter threshold applied after summation. 0.95 default
     * is the `peak_limiter` default too. Set to 1.0f to effectively
     * disable (samples can still clip if raw sum exceeds ±1). */
    float           peak_threshold  = 0.95f;
};

class AudioMixer {
public:
    /* Construct with a config; target_ch_layout is copied internally
     * (caller may uninit theirs after). Validates FLTP target + frame_size > 0
     * — on invalid config, err populated and `ok()` returns false. */
    AudioMixer(const AudioMixerConfig& cfg, std::string* err);
    ~AudioMixer();

    AudioMixer(const AudioMixer&)            = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&)                 = default;
    AudioMixer& operator=(AudioMixer&&)      = default;

    bool ok() const noexcept { return ok_; }

    /* Add a track. Feed's target config must match mixer's
     * (target_rate, target_fmt, target_ch_layout.nb_channels).
     * Returns ME_OK on success; ME_E_INVALID_ARG if mismatch; in
     * either case the feed is consumed (moved in on success;
     * destructed on failure). */
    me_status_t add_track(AudioTrackFeed feed, std::string* err);

    /* Pull the next mixed frame of `cfg.frame_size` samples.
     *
     * On success (ME_OK): `*out_frame` is a freshly-allocated
     * AVFrame at (target_rate, target_fmt, target_ch_layout,
     * nb_samples=frame_size), samples mixed + peak-limited.
     * Caller owns — free via `av_frame_free(&f)`.
     *
     * Returns ME_E_NOT_FOUND when all tracks are drained AND all
     * FIFOs are empty (full EOF — no more output possible).
     *
     * On error: returns ME_E_INTERNAL / ME_E_OUT_OF_MEMORY with
     * err populated.
     *
     * Partial-drain semantic: if some tracks drain early, mixer
     * continues emitting frames — drained tracks contribute silence
     * (zero samples) to the sum so the mix timeline continues to
     * the last-alive track's EOF. */
    me_status_t pull_next_mixed_frame(AVFrame**    out_frame,
                                       std::string* err);

    /* True when all feeds are drained AND all FIFOs empty. */
    bool eof() const noexcept;

    std::size_t track_count() const noexcept { return tracks_.size(); }

    /* TESTING ONLY — inject raw samples directly into track `ti`'s
     * per-plane FIFO, bypassing the AudioTrackFeed decode/resample/
     * gain stages. Lets unit tests drive known waveforms (sine /
     * DC / saturation) through the mix + peak_limit path without
     * synthesizing real AAC fixtures. Production code must use
     * `add_track` + let the feed pull from a real demux.
     *
     * Layout: `plane_data[ch][i]` = sample `i` on channel `ch`, for
     * `i in [0, num_samples)` and `ch in [0, num_channels())`. Samples
     * append to the end of that plane's FIFO.
     *
     * Returns ME_E_INVALID_ARG on null args / bad track index /
     * mismatched num_channels. */
    me_status_t inject_samples_for_test(std::size_t         ti,
                                         const float* const* plane_data,
                                         std::size_t         num_samples,
                                         std::string*        err);

private:
    struct TrackState {
        AudioTrackFeed feed;
        /* Per-channel-plane FIFO. Size = cfg.target_ch_layout.nb_channels.
         * Each plane holds queued float samples; front is the next
         * sample to emit. */
        std::vector<std::vector<float>> planes;
    };

    /* Pull one AVFrame from track[ti].feed, resample/gain applied
     * (feed does it), append its samples to track[ti].planes.
     * Returns ME_OK, ME_E_NOT_FOUND (feed EOF), or error. */
    me_status_t fill_one_from_feed(TrackState& ts, std::string* err);

    AudioMixerConfig cfg_{};
    bool             ok_ = false;
    int              num_channels_ = 0;
    std::vector<TrackState> tracks_;
};

/* Build an AudioMixer from a Timeline + per-clip DemuxContext list.
 *
 * Walks `tl.clips`, filters to clips whose track has
 * `TrackKind::Audio`, opens an `AudioTrackFeed` per such clip using
 * `demux_by_clip_idx[clip_idx]` as the source demux and applying
 * the clip's `gain_db` (converted via `db_to_linear`) as the
 * feed's linear gain (clips without gain_db default to 0 dB = unity).
 * Each feed is added to the new mixer.
 *
 * `demux_by_clip_idx` must be indexed parallel to `tl.clips` —
 * i.e. `demux_by_clip_idx[ci]` is the opened demux for `tl.clips[ci]`
 * (same convention as ComposeSink's `demuxes` parameter). Video
 * clips' slots in this vector are ignored; audio clips' slots must
 * be non-null.
 *
 * Returns:
 *   - ME_OK + `out` populated on success (at least 1 audio clip found).
 *   - ME_E_NOT_FOUND if no audio clips in the timeline.
 *   - ME_E_INVALID_ARG on null args / mismatched vector size / null demux
 *     for an audio clip / empty `tl.clips` / empty `tl.tracks`.
 *   - Propagates errors from `AudioMixer` construction or
 *     `open_audio_track_feed` if decoder setup fails.
 *
 * This helper is scope-A-carved for the coming ComposeSink audio
 * path rewrite: the sink will call this builder to construct the
 * mix pipeline, then pull mixed frames into the AAC encoder. */
me_status_t build_audio_mixer_for_timeline(
    const me::Timeline&                                         tl,
    me::resource::CodecPool&                                    pool,
    const std::vector<std::shared_ptr<me::io::DemuxContext>>&   demux_by_clip_idx,
    const AudioMixerConfig&                                     cfg,
    std::unique_ptr<AudioMixer>&                                out,
    std::string*                                                err);

}  // namespace me::audio
