/*
 * compose::affine_blit kernel registration.
 *
 * Registers TaskKindId::RenderAffineBlit. Wraps the pure
 * `compose_inverse_affine` + `affine_blit` helpers (src/compose/affine_blit.hpp)
 * into the graph kernel ABI so per-clip Transform application becomes a
 * scheduled Node instead of an inline call inside the compose loop.
 *
 * Schema:
 *   inputs:  [src: RgbaFrame]
 *   outputs: [dst: RgbaFrame]
 *   params:
 *     translate_x, translate_y         (Float64, optional, default 0)
 *     scale_x, scale_y                 (Float64, optional, default 1)
 *     rotation_deg                     (Float64, optional, default 0)
 *     anchor_x, anchor_y               (Float64, optional, default 0)
 *     dst_w, dst_h                     (Int64,   required — canvas size)
 *
 * time_invariant = true: output is fully determined by input pixels +
 * params; same input + same params cache to the same output.
 */
#pragma once

namespace me::compose {
void register_affine_blit_kind();
}
