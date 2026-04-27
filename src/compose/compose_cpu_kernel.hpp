/*
 * compose::compose_cpu kernel registration.
 *
 * Registers TaskKindId::RenderComposeCpu. N×RgbaFrame → 1×RgbaFrame
 * multi-layer composite via Porter-Duff source-over (with optional
 * blend modes) — wraps the existing me::compose::alpha_over helper
 * into the graph kernel ABI so multi-track compose becomes a single
 * scheduled Node instead of an inline loop in the orchestrator.
 *
 * Schema:
 *   inputs:  [layer_0: RgbaFrame, layer_1: RgbaFrame, ...]   — variadic
 *            (Builder enforces ≥1; layer_0 = bottom of z-order)
 *   outputs: [composite: RgbaFrame]
 *   params:
 *     dst_w, dst_h                    (Int64, required — canvas size)
 *     opacity_<i>                     (Float64, default 1.0; i = 0..N-1)
 *     blend_mode_<i>                  (Int64,   default 0   — 0=Normal,
 *                                                              1=Multiply,
 *                                                              2=Screen)
 *
 * All inputs must already be at canvas dimensions (dst_w × dst_h);
 * upstream callers run RenderAffineBlit per layer to resize/transform
 * each into canvas space before this kernel.
 *
 * time_invariant = true: output is fully determined by input pixels +
 * params. cacheable = true (default).
 *
 * The variadic input port count is not encoded in input_schema (we
 * register a single repeating "layer" port shape); Builder validates
 * type per actual port at graph build time.
 */
#pragma once

namespace me::compose {
void register_compose_cpu_kind();
}
