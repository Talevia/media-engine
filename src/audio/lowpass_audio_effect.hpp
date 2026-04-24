/*
 * me::audio::LowpassAudioEffect — one-pole IIR low-pass filter,
 * per-channel stateful.
 *
 * Basic EQ building block — foundation for the "基础 EQ" half of
 * the M4 "Audio effect chain (gain / pan / 基础 EQ)" criterion.
 * Simplest filter that's not a no-op: y[n] = a * x[n] + (1-a) *
 * y[n-1], where the coefficient `a` is derived from the desired
 * cutoff frequency and the sample rate each process() call.
 *
 *   a = 1 - exp(-2π * fc / fs)
 *
 * This is the standard RC-low-pass discrete analog — well known,
 * cheap (one mul + one add per sample per channel), stable, and
 * has a recognizable 6 dB/octave rolloff.
 *
 * State: per-channel `y_prev` buffer (up to 8 channels). Cleared
 * on `reset()`; the caller invokes reset at stream boundaries
 * (clip seek, mixer restart). Chained with other AudioEffects
 * freely — AudioEffectChain doesn't propagate reset automatically,
 * so the downstream integration will need to call it explicitly
 * when its domain expects discontinuity.
 *
 * Future EQ types (high-pass, band-pass, shelving, parametric)
 * land as sibling AudioEffects once real needs surface.
 */
#pragma once

#include "audio/audio_effect.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace me::audio {

class LowpassAudioEffect final : public AudioEffect {
public:
    /* cutoff_hz: desired cutoff; 20 Hz ≤ fc ≤ sample_rate/2.
     * Caller may adjust via set_cutoff(). */
    explicit LowpassAudioEffect(float cutoff_hz = 1000.0f) noexcept
        : cutoff_hz_(cutoff_hz) {}

    void  set_cutoff(float cutoff_hz) noexcept { cutoff_hz_ = cutoff_hz; }
    float cutoff() const noexcept              { return cutoff_hz_; }

    /* Clear per-channel filter state. Call at stream boundaries
     * (clip seek, mixer reset) to avoid one-sample click from
     * stale y_prev. */
    void reset() noexcept {
        for (auto& v : y_prev_) v = 0.0f;
    }

    void process(float*       samples,
                 std::size_t  n_frames,
                 int          n_channels,
                 int          sample_rate) override {
        if (n_channels <= 0 || n_channels > 8 || sample_rate <= 0) return;

        /* a = 1 - exp(-2π * fc / fs). Clamp fc to (0, fs/2) so a
         * stays in (0, 1] and the filter stays stable. */
        const float fc_clamped = std::clamp(cutoff_hz_, 1.0f,
                                             static_cast<float>(sample_rate) * 0.5f);
        const float two_pi     = 6.28318530717958647692f;
        const float a          = 1.0f - std::exp(-two_pi * fc_clamped /
                                                  static_cast<float>(sample_rate));
        const float one_minus_a = 1.0f - a;

        for (std::size_t i = 0; i < n_frames; ++i) {
            for (int c = 0; c < n_channels; ++c) {
                const float x = samples[i * n_channels + c];
                const float y = a * x + one_minus_a * y_prev_[c];
                y_prev_[c]               = y;
                samples[i * n_channels + c] = y;
            }
        }
    }

    const char* kind() const noexcept override { return "lowpass"; }

private:
    float cutoff_hz_;
    /* Per-channel state; up to 8 (5.1 + LFE + a pair of margin
     * channels) covers every container codec we support. Static
     * array keeps the effect allocation-free. */
    float y_prev_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

}  // namespace me::audio
