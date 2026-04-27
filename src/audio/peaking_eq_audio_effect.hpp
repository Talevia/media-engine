/*
 * me::audio::PeakingEqAudioEffect — parametric peaking EQ.
 *
 * Real parametric-EQ band sibling to LowpassAudioEffect. Three
 * params per band:
 *
 *   freq_hz   — centre frequency of the boost / cut.
 *   gain_db   — peak gain at centre. Positive = boost, negative =
 *               cut, 0 = identity (filter degenerates to unity).
 *   q         — quality factor; higher Q narrows the bandwidth
 *               around freq_hz. Typical music-EQ values: 0.7..3.0.
 *
 * Implementation is the canonical biquad peaking EQ from Robert
 * Bristow-Johnson's "Audio EQ Cookbook" (https://www.w3.org/TR/audio-eq-cookbook/),
 * stored in direct-form-I (one set of x_prev / y_prev pairs per
 * channel). Coefficients recomputed lazily when any of (freq_hz,
 * gain_db, q, sample_rate) changes — which means cheap steady
 * state and at-most-one trig pass per process() call after a
 * parameter change.
 *
 *   A      = 10^(gain_db / 40)
 *   ω      = 2π · freq_hz / sample_rate
 *   α      = sin(ω) / (2·q)
 *
 *   b0 = 1 + α·A          a0 = 1 + α/A
 *   b1 = -2·cos(ω)        a1 = -2·cos(ω)
 *   b2 = 1 - α·A          a2 = 1 - α/A
 *
 *   Normalised:  b{0,1,2} /= a0;  a{1,2} /= a0;  a0 := 1.
 *
 * Per-channel state: x_prev[2] + y_prev[2] (up to 8 channels);
 * 64 floats total — allocation-free, same shape as LowpassAudioEffect.
 *
 * Determinism: pure scalar float arithmetic, no parallelism, no
 * SIMD intrinsics. Same inputs → same bytes — satisfies VISION §3.1.
 *
 * Reset semantics: identical to LowpassAudioEffect — caller invokes
 * `reset()` at stream boundaries (clip seek, mixer restart) so the
 * filter doesn't carry a click from the prior buffer's tail.
 */
#pragma once

#include "audio/audio_effect.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace me::audio {

class PeakingEqAudioEffect final : public AudioEffect {
public:
    /* Default values produce a 0 dB band at 1 kHz with Q=1.0 — i.e.
     * an inaudible identity. Any non-zero gain_db activates the
     * filter; setting gain_db back to 0 returns to identity (the
     * filter still runs, but the output equals the input within
     * float epsilon for typical signal levels). */
    explicit PeakingEqAudioEffect(float freq_hz = 1000.0f,
                                   float gain_db = 0.0f,
                                   float q       = 1.0f) noexcept
        : freq_hz_(freq_hz), gain_db_(gain_db), q_(q) {}

    void  set_freq_hz(float v) noexcept { if (v != freq_hz_) { freq_hz_ = v; coeffs_dirty_ = true; } }
    void  set_gain_db(float v) noexcept { if (v != gain_db_) { gain_db_ = v; coeffs_dirty_ = true; } }
    void  set_q      (float v) noexcept { if (v != q_)       { q_       = v; coeffs_dirty_ = true; } }

    float freq_hz()  const noexcept { return freq_hz_; }
    float gain_db()  const noexcept { return gain_db_; }
    float q()        const noexcept { return q_; }

    /* Clear per-channel filter state. Same purpose as
     * LowpassAudioEffect::reset — clears one-sample click after a
     * stream discontinuity. */
    void reset() noexcept {
        for (auto& v : x1_) v = 0.0f;
        for (auto& v : x2_) v = 0.0f;
        for (auto& v : y1_) v = 0.0f;
        for (auto& v : y2_) v = 0.0f;
    }

    void process(float*       samples,
                 std::size_t  n_frames,
                 int          n_channels,
                 int          sample_rate) override {
        if (n_channels <= 0 || n_channels > 8 || sample_rate <= 0) return;

        if (coeffs_dirty_ || sample_rate != cached_sr_) {
            recompute_coeffs(sample_rate);
            cached_sr_     = sample_rate;
            coeffs_dirty_  = false;
        }

        const float b0 = b0_, b1 = b1_, b2 = b2_;
        const float a1 = a1_, a2 = a2_;

        for (std::size_t i = 0; i < n_frames; ++i) {
            for (int c = 0; c < n_channels; ++c) {
                const float x = samples[i * n_channels + c];
                /* Direct-form I:
                 *   y = b0·x + b1·x_prev1 + b2·x_prev2
                 *         - a1·y_prev1 - a2·y_prev2  (a0 already 1) */
                const float y = b0 * x
                              + b1 * x1_[c]
                              + b2 * x2_[c]
                              - a1 * y1_[c]
                              - a2 * y2_[c];
                x2_[c] = x1_[c];
                x1_[c] = x;
                y2_[c] = y1_[c];
                y1_[c] = y;
                samples[i * n_channels + c] = y;
            }
        }
    }

    const char* kind() const noexcept override { return "peaking_eq"; }

private:
    void recompute_coeffs(int sample_rate) noexcept {
        /* Clamp params to physically meaningful ranges so a host
         * passing junk (q=0 → division by zero, freq>nyquist →
         * filter blow-up) degrades gracefully rather than producing
         * NaNs. */
        const float fc_clamped = std::clamp(freq_hz_, 1.0f,
                                             static_cast<float>(sample_rate) * 0.5f);
        const float q_clamped  = std::clamp(q_, 0.1f, 32.0f);

        const float two_pi = 6.28318530717958647692f;
        const float A     = std::pow(10.0f, gain_db_ * (1.0f / 40.0f));
        const float omega = two_pi * fc_clamped / static_cast<float>(sample_rate);
        const float cw    = std::cos(omega);
        const float sw    = std::sin(omega);
        const float alpha = sw / (2.0f * q_clamped);

        const float a0 = 1.0f + alpha / A;
        const float inv_a0 = 1.0f / a0;

        b0_ = (1.0f + alpha * A) * inv_a0;
        b1_ = (-2.0f * cw)        * inv_a0;
        b2_ = (1.0f - alpha * A) * inv_a0;
        a1_ = (-2.0f * cw)        * inv_a0;
        a2_ = (1.0f - alpha / A) * inv_a0;
    }

    float freq_hz_;
    float gain_db_;
    float q_;

    /* Cached coefficients + state. coeffs_dirty_ flips when any
     * setter changes a parameter; cached_sr_ tracks sample rate so
     * a process() call at a new rate re-derives. */
    bool  coeffs_dirty_ = true;
    int   cached_sr_    = -1;
    float b0_ = 0, b1_ = 0, b2_ = 0;
    float a1_ = 0, a2_ = 0;

    /* Per-channel filter memory — direct-form-I needs two x history
     * + two y history per channel. 8 channels covers up to 7.1; the
     * static array keeps the effect allocation-free, same as Lowpass. */
    float x1_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float x2_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float y1_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float y2_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

}  // namespace me::audio
