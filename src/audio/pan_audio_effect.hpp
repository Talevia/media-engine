/*
 * me::audio::PanAudioEffect — stereo L/R balance.
 *
 * `pan` ∈ [-1, +1]:
 *   -1 = full left  (R = 0)
 *    0 = center     (L and R unchanged)
 *   +1 = full right (L = 0)
 *
 * Linear pan law — L / R gains are linear functions of `pan`.
 * This is the simplest model; a more audibly-even constant-power
 * pan (cos/sin) is a future refinement if perceptual neutrality
 * at center matters for a real workload.
 *
 * Non-stereo inputs: process() is a no-op when n_channels != 2.
 * The AudioEffect contract says effects shouldn't throw on
 * misapplication — silence is friendlier than a crash, and the
 * host can check `kind()` before installing on a non-stereo track.
 */
#pragma once

#include "audio/audio_effect.hpp"

namespace me::audio {

class PanAudioEffect final : public AudioEffect {
public:
    explicit PanAudioEffect(float pan = 0.0f) noexcept : pan_(pan) {}

    void set_pan(float pan) noexcept { pan_ = pan; }
    float pan() const noexcept       { return pan_; }

    void process(float*       samples,
                 std::size_t  n_frames,
                 int          n_channels,
                 int          /*sample_rate*/) override {
        if (n_channels != 2) return;  // stereo-only; see header note

        /* Linear pan law:
         *   left_gain  = 1 - max(0, pan)
         *   right_gain = 1 - max(0, -pan) = 1 + min(0, pan)
         * pan=0  → (1, 1); pan=+1 → (0, 1); pan=-1 → (1, 0). */
        const float p  = pan_;
        const float lg = (p > 0.0f) ? (1.0f - p) : 1.0f;
        const float rg = (p < 0.0f) ? (1.0f + p) : 1.0f;

        for (std::size_t i = 0; i < n_frames; ++i) {
            samples[i * 2 + 0] *= lg;
            samples[i * 2 + 1] *= rg;
        }
    }

    const char* kind() const noexcept override { return "pan"; }

private:
    float pan_;
};

}  // namespace me::audio
