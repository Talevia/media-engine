/*
 * me::audio::GainAudioEffect — scalar amplitude multiplier.
 *
 * One of the three phase-1 AudioEffect implementations. Simplest
 * possible: multiply every interleaved sample by a linear gain
 * factor. The existing Clip::gain_db (timeline_impl.hpp) remains
 * the per-clip-keyframed path; this class is the reusable
 * building block for explicit effect chains on a clip.
 *
 * Contract: gain = 1.0 = pass-through; gain = 0.0 = silence.
 * Negative gain = phase invert (valid audio engineering op).
 * No clipping — caller's responsibility to scale before any
 * fixed-point stage downstream.
 */
#pragma once

#include "audio/audio_effect.hpp"

namespace me::audio {

class GainAudioEffect final : public AudioEffect {
public:
    explicit GainAudioEffect(float gain = 1.0f) noexcept : gain_(gain) {}

    void set_gain(float gain) noexcept { gain_ = gain; }
    float gain() const noexcept        { return gain_; }

    void process(float*       samples,
                 std::size_t  n_frames,
                 int          n_channels,
                 int          /*sample_rate*/) override {
        const std::size_t n = n_frames * static_cast<std::size_t>(n_channels);
        for (std::size_t i = 0; i < n; ++i) {
            samples[i] *= gain_;
        }
    }

    const char* kind() const noexcept override { return "gain"; }

private:
    float gain_;
};

}  // namespace me::audio
