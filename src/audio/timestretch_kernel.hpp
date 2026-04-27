/*
 * audio::timestretch kernel registration.
 *
 * Registers TaskKindId::AudioTimestretch. 1×AvFrameHandle (audio
 * AVFrame, FLTP) → 1×AvFrameHandle (tempo-stretched AVFrame, FLTP).
 * Wraps me::audio::TempoStretcher (SoundTouch backend) into the
 * graph kernel ABI.
 *
 * Schema:
 *   inputs:  [src: AvFrameHandle]      (audio AVFrame, FLTP)
 *   outputs: [dst: AvFrameHandle]      (audio AVFrame, FLTP, may have
 *                                       fewer/more nb_samples than src)
 *   params:
 *     tempo          (Float64, required — 1.0 = pass-through, 2.0 = 2× faster)
 *     instance_key   (Int64,   default 0  — stable per-track id;
 *                     same key across chunks reuses the SoundTouch
 *                     instance + its internal pitch-buffer state)
 *
 * State continuity across chunks:
 *   The kernel borrows a TempoStretcher from
 *   ctx.tempo_pool keyed by instance_key. SoundTouch holds a pitch
 *   buffer that smooths cross-chunk transitions; preserving that
 *   buffer across kernel calls (vs. re-allocating per call) is what
 *   makes timestretch sound natural rather than zippered. When
 *   ctx.tempo_pool is null (compile-time absent or test contexts),
 *   the kernel falls back to a fresh-per-call TempoStretcher —
 *   functionally correct but with chunk-boundary artifacts.
 *
 *   Output sample count is whatever SoundTouch chose to emit this
 *   call (often less than input on the first chunk while it fills
 *   the pitch buffer; more once the pipeline is primed). Caller
 *   handles concatenation across chunks.
 *
 * cacheable = false: output depends on the borrowed instance's
 * internal state, which mutates per call. Two evaluations with the
 * same input + instance_key produce different bytes (the second
 * sees the buffered tail from the first).
 *
 * time_invariant = false (output not purely a function of inputs +
 * params at this kernel's level — the per-chunk advance through
 * SoundTouch's buffer is the implicit time variable).
 */
#pragma once

namespace me::audio {
void register_timestretch_kind();
}
