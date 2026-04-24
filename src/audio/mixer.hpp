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

#include <string>
#include <vector>

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

}  // namespace me::audio
