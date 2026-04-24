/*
 * me::audio::AudioEffect — abstract base for audio signal-processing
 * effects.
 *
 * M4 exit criterion "Audio effect chain (gain / pan / 基础 EQ)"
 * foundation. Parallel structure to the video Effect/EffectChain
 * pair in src/effect/: AudioEffect processes interleaved float
 * PCM samples in place. Concrete first-batch implementations land
 * in sibling TUs:
 *   - GainAudioEffect — scalar amplitude multiplier
 *   - PanAudioEffect  — linear L/R balance (stereo only)
 *   - LowpassAudioEffect — one-pole IIR filter, base for future EQ
 *
 * Contract:
 *   - `process(samples, n_frames, n_channels, sample_rate)`:
 *     in-place over `n_frames * n_channels` interleaved floats.
 *     Sample rate is a hint — most effects ignore it (gain / pan
 *     are rate-invariant); filters use it for cutoff computation.
 *   - Must be deterministic: same input → same bytes. No rand,
 *     no parallelism, no non-associative float reductions unless
 *     documented.
 *   - Must not throw. Internal errors (zero buffer, bogus params)
 *     degrade to no-op rather than surface to the C API (audio
 *     glitches > crash).
 *
 * Non-goals for phase-1:
 *   - Multi-pass / tail-producing effects (reverb, delay). Those
 *     need lookahead + output buffers and don't fit the in-place
 *     contract.
 *   - Variable channel-count effects (stereo → surround upmix).
 *   - Stateful effects that need per-clip reset on seek — every
 *     phase-1 effect is stateless OR state tracks per-instance,
 *     not per-sample-stream.
 */
#pragma once

#include <cstddef>

namespace me::audio {

class AudioEffect {
public:
    virtual ~AudioEffect() = default;

    /* Apply to an interleaved float PCM buffer in place.
     *   samples    : float array of length n_frames * n_channels
     *   n_frames   : per-channel frame count
     *   n_channels : interleaved channel count
     *   sample_rate: samples/second/channel (hint; rate-invariant
     *                effects may ignore it). */
    virtual void process(float*       samples,
                         std::size_t  n_frames,
                         int          n_channels,
                         int          sample_rate) = 0;

    /* Debug-visible tag. Used by logs + tests to verify chain
     * ordering / identity without type introspection. */
    virtual const char* kind() const noexcept = 0;
};

}  // namespace me::audio
