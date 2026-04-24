/*
 * me::audio::TempoStretcher — tempo (speed) change without
 * pitch shift, via SoundTouch 2.4.0.
 *
 * M4 exit criterion "SoundTouch 集成，支持变速不变调" foundation.
 * Consumer: future per-clip tempo animation (tempo keyframes,
 * eventually wired through AudioMixer's sample-emission path).
 * This first landing is the wrapper + a self-contained correctness
 * test; mixer integration lands in a follow-up cycle when a real
 * time-stretch consumer surfaces.
 *
 * Compiled only when the build is configured with
 * `-DME_WITH_SOUNDTOUCH=ON` (default ON). Source not guarded
 * internally — compile-time gating lives in the CMake generator
 * expression on src/audio/tempo.cpp.
 *
 * Contract:
 *   - Construct with sample rate + channel count; these are
 *     immutable for the stretcher's lifetime.
 *   - `set_tempo(t)` — t > 0; 1.0 = pass-through, 2.0 = half as
 *     long, 0.5 = twice as long. Safe to call between process
 *     calls; SoundTouch handles the transition via its own
 *     look-ahead buffer.
 *   - `put_samples(interleaved, n_frames)` — feed `n_frames ×
 *     channel_count` floats. `receive_samples(out, cap_frames)`
 *     — pull up to `cap_frames` frames; returns actual count.
 *   - `flush()` — signal end of input; drains SoundTouch's
 *     internal buffer so all remaining tempo-stretched output
 *     can be pulled via receive_samples.
 *
 * Threading: not thread-safe. One SoundTouch instance owns
 * substantial internal state (FIFO + FFT scratch); callers
 * must serialize or use one stretcher per-thread.
 */
#pragma once

#include <cstddef>
#include <memory>

namespace soundtouch { class SoundTouch; }

namespace me::audio {

class TempoStretcher {
public:
    TempoStretcher(int sample_rate, int channels);
    ~TempoStretcher();

    TempoStretcher(const TempoStretcher&)            = delete;
    TempoStretcher& operator=(const TempoStretcher&) = delete;
    TempoStretcher(TempoStretcher&&) noexcept;
    TempoStretcher& operator=(TempoStretcher&&) noexcept;

    /* Tempo multiplier. 1.0 = no change; 2.0 = output half the
     * length (faster); 0.5 = double length (slower). SoundTouch
     * supports a wide range but extreme values degrade quality;
     * [0.25, 4.0] is the documented useful band. No clamp here —
     * caller is responsible. */
    void set_tempo(double tempo);

    /* Feed `n_frames` frames of interleaved float samples.
     * `samples` points to `n_frames * channels` elements. */
    void put_samples(const float* samples, std::size_t n_frames);

    /* Pull up to `cap_frames` frames of output into `out`
     * (interleaved). Returns actual number of frames written.
     * May return 0 if SoundTouch hasn't accumulated enough
     * input yet — keep pulling after more put_samples calls. */
    std::size_t receive_samples(float* out, std::size_t cap_frames);

    /* Signal end of input — tells SoundTouch to emit any tail
     * buffer through receive_samples. Idempotent; subsequent
     * put_samples after flush re-arms the pipeline. */
    void flush();

    int sample_rate() const noexcept { return sample_rate_; }
    int channels()    const noexcept { return channels_;    }

private:
    int sample_rate_ = 0;
    int channels_    = 0;
    std::unique_ptr<soundtouch::SoundTouch> st_;
};

}  // namespace me::audio
