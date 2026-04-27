/*
 * compose::cross_dissolve kernel registration.
 *
 * Registers TaskKindId::RenderCrossDissolve. 2×RgbaFrame + progress
 * → 1×RgbaFrame. Wraps the existing me::compose::cross_dissolve
 * helper into the graph kernel ABI so transition rendering becomes
 * a scheduled Node — used by compile_frame_graph (commit 4) when the
 * current frame falls inside a transition window between two clips.
 *
 * Schema:
 *   inputs:  [from: RgbaFrame, to: RgbaFrame]
 *   outputs: [out:  RgbaFrame]
 *   params:
 *     progress     (Float64, required, clamped [0, 1])
 *
 * progress = 0 → output bytes-identical to `from`; progress = 1 →
 * output bytes-identical to `to`. Linear lerp per channel; alpha is
 * also lerped (not screen-blended).
 *
 * time_invariant = true: same `from` + `to` + `progress` always
 * produce the same bytes, so the OutputCache key (content_hash mixed
 * with ctx.time when not time_invariant) collapses to content_hash
 * alone. Different progress values produce different content_hashes
 * via Properties hashing, so per-frame variation is captured without
 * needing ctx.time mixing.
 */
#pragma once

namespace me::compose {
void register_cross_dissolve_kind();
}
