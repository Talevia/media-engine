/*
 * audio::mix kernel registration.
 *
 * Registers TaskKindId::AudioMix. N×AvFrameHandle (audio AVFrames,
 * FLTP, all matching rate/channels/sample count) → 1×AvFrameHandle
 * (mixed FLTP). Wraps me::audio::mix_samples + peak_limiter into the
 * graph kernel ABI.
 *
 * Schema:
 *   inputs:  [track_0, track_1, ...]   variadic, all AvFrameHandle
 *   outputs: [mixed: AvFrameHandle]
 *   params:
 *     gain_db_<i>             (Float64, default 0.0)         — per-track gain, dB
 *     peak_limit_threshold    (Float64, default 0.95)        — soft-knee, 0 = disabled
 *
 * All inputs must match on:
 *   - sample format (must be AV_SAMPLE_FMT_FLTP — phase-1 mixer constraint)
 *   - sample rate
 *   - channel count
 *   - nb_samples
 * Upstream RenderResample brings each track to a common
 * (rate, fmt, channels) before mix; AudioMix kernel rejects
 * mismatched inputs with ME_E_INVALID_ARG (deterministic + obvious
 * failure rather than producing silently-wrong audio).
 *
 * time_invariant=true + cacheable=true: pure float math, deterministic.
 */
#pragma once

namespace me::audio {
void register_mix_kind();
}
