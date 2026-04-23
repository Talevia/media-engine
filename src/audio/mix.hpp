/*
 * me::audio::mix — pure sample-level mixing math for multi-track audio.
 *
 * Lands as part of the `audio-mix-kernel` cycle's first slice (the
 * bullet itself estimates 3-4 cycles total); sits in `src/audio/` as
 * the canonical mixing primitives. Future cycles add libswresample
 * wiring (resample N inputs to common rate), per-clip gainDb → linear
 * conversion, AVFrame-level scheduling, and sink integration.
 *
 * Signal convention (this file):
 *   - Samples are `float` in [-1.0, +1.0] (clamped by limiter, not
 *     hard-clipped inside mix_samples).
 *   - Interleaved channel layout is out of scope — these helpers work
 *     on single-channel strips; the eventual AudioMixPipeline iterates
 *     per channel and calls these primitives.
 *   - No NaN / Inf handling: callers must sanitise upstream (typical
 *     decoder output is finite).
 *
 * Determinism: pure IEEE-754 float32 addition + tanh approximation,
 * no FMA, no SIMD (yet). Same inputs in → same bytes out across hosts
 * (VISION §5.3). Future SIMD path must prove determinism or go behind
 * an explicit feature flag.
 */
#pragma once

#include <cstddef>

namespace me::audio {

/* Mix N per-track single-channel input strips into a single output
 * strip, scaling each input by the matching gain_linear[i].
 *
 *   output[j] = sum_i (inputs[i][j] * gain_linear[i])   for j in [0, num_samples)
 *
 * - `inputs` is a pointer to num_inputs float strips, each at least
 *   num_samples long.
 * - `gain_linear` is a linear amplitude scaler per input; pass 1.0
 *   for unity, 0.0 for silence, 0.5 for −6 dB (pre-compute from
 *   gainDb via `10^(db/20)` at the caller).
 * - `output` receives the sum; must be at least num_samples long.
 *   Aliasing with any input strip is UB (callers must own a
 *   separate buffer).
 * - num_inputs == 0 writes silence (zeros) to output.
 *
 * Caller is responsible for clamping / limiting the sum; this function
 * produces raw sums that may exceed ±1.0 when inputs overlap. Use
 * `peak_limiter` downstream when overload is a concern. */
void mix_samples(const float* const* inputs,
                 const float*        gain_linear,
                 std::size_t         num_inputs,
                 std::size_t         num_samples,
                 float*              output);

/* Simple soft-knee peak limiter. In-place on `samples`:
 *
 *   |x| <= threshold   →  y = x
 *   |x| >  threshold   →  y = sign(x) * (threshold + (1 - threshold) *
 *                                         tanh((|x| - threshold) /
 *                                              (1 - threshold)))
 *
 * This keeps the <threshold region linear-pass-through and smoothly
 * compresses overages toward ±1.0 via tanh. threshold defaults to
 * 0.95; callers who want harder / softer knees pass their own value
 * in [0.5, 0.99].
 *
 * Returns the peak |x| observed before limiting — useful for meter
 * UI / headroom stats. */
float peak_limiter(float*      samples,
                   std::size_t num_samples,
                   float       threshold = 0.95f);

/* Convert a gain in decibels to linear amplitude scaling:
 *   linear = 10^(db / 20)
 * 0 dB → 1.0; −6 dB → ≈0.501; −∞ dB → 0.0 (caller passes -INFINITY
 * for true silence, which returns 0.0 exactly). */
float db_to_linear(float db);

}  // namespace me::audio
