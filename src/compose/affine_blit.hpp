/*
 * me::compose::affine_blit — 2D affine transform for RGBA8 buffers.
 *
 * The compose loop needs to apply per-track Transform (translate,
 * scale, rotate, anchor) to source frames before alpha_over-ing them
 * onto the canvas. This file provides the pure pixel-level kernel
 * plus a helper to build the inverse affine matrix from Transform
 * parameters. The ComposeSink-side wiring lands with
 * compose-transform-affine-wire.
 *
 * Sampling: nearest-neighbor. Bilinear is a future optimization
 * (better quality at slight perf cost and ~3× more arithmetic per
 * pixel). For phase-1 correctness, nearest covers the canonical
 * transforms (identity, translate, scale, 90° rotate) without
 * numerical drift.
 *
 * Determinism: pure IEEE-754 float32 math with lroundf for uint8
 * rounding and nearest-integer sampling index computation. No FMA /
 * SIMD. Same inputs → same bytes across hosts.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Row-major 2×3 affine matrix: [a, b, tx; c, d, ty].
 * Maps (x, y) to (a*x + b*y + tx, c*x + d*y + ty).
 *
 * The blit kernel uses the INVERSE of the forward transform: it
 * iterates dst pixels and asks "which src pixel does this come
 * from?". compose_inverse_affine builds that inverse directly from
 * Transform semantics. */
struct AffineMatrix {
    float a  = 1.0f, b  = 0.0f, tx = 0.0f;
    float c  = 0.0f, d  = 1.0f, ty = 0.0f;
};

/* Build the inverse affine mapping (canvas → src) from Transform
 * parameters. The forward transform semantics (src → canvas):
 *   1. Subtract anchor offset (anchor_x * src_w, anchor_y * src_h)
 *   2. Scale by (scale_x, scale_y)
 *   3. Rotate by rotation_deg clockwise
 *   4. Add anchor offset back
 *   5. Translate by (translate_x, translate_y)
 *
 * The inverse undoes these in reverse order. Returned matrix is
 * directly usable by affine_blit.
 *
 * Degenerate case: scale_x == 0 or scale_y == 0 → inverse is
 * singular; the matrix stays identity so the caller can detect
 * (src_w/src_h won't map meaningfully; output will be
 * transparent). In practice loader validates scale > 0 is the
 * common case; zero scale means "fully hidden" which is better
 * represented via opacity=0. */
AffineMatrix compose_inverse_affine(double translate_x,
                                     double translate_y,
                                     double scale_x,
                                     double scale_y,
                                     double rotation_deg,
                                     double anchor_x,
                                     double anchor_y,
                                     int    src_w,
                                     int    src_h);

/* Blit `src` onto `dst` under the given inverse affine mapping.
 * For each dst pixel (x, y), computes (sx, sy) = inv * (x, y) via
 * nearest-neighbor round, samples src at (sx, sy) if in bounds,
 * writes to dst[x, y]. Out-of-bounds reads write {0, 0, 0, 0}
 * (transparent black) to dst — caller's alpha_over pass will treat
 * that as "src contributes nothing at this pixel" and preserve the
 * lower composite layer.
 *
 * `dst` and `src` must not alias. Both are row-major RGBA8.
 * Stride parameters are in bytes. */
void affine_blit(uint8_t*           dst,
                 int                dst_w,
                 int                dst_h,
                 std::size_t        dst_stride_bytes,
                 const uint8_t*     src,
                 int                src_w,
                 int                src_h,
                 std::size_t        src_stride_bytes,
                 const AffineMatrix& inv);

}  // namespace me::compose
