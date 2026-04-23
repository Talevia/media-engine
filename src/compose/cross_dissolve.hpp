/*
 * me::compose::cross_dissolve — pixel-level alpha ramp mix for the
 * cross-dissolve transition.
 *
 * Lands as part of the `cross-dissolve-kernel` cycle's first slice
 * (same pattern as `alpha_over` was for `multi-track-compose-kernel`).
 * This file provides the pure mixing math; the transition scheduler
 * (decode from-clip tail + to-clip head + drive per-frame t values
 * against the transition duration + feed compose-sink) is deferred
 * to a follow-up bullet.
 *
 * Shape:
 *   - Three RGBA8 buffers: `from` (outgoing clip), `to` (incoming
 *     clip), `dst` (output). All three are tightly-packed, same
 *     width × height × stride.
 *   - `t` in [0, 1]: 0 means dst == from (transition just starting),
 *     1 means dst == to (transition complete). Linear lerp per
 *     channel.
 *   - In-place dst; aliasing `dst` with either source buffer is UB.
 *   - Alpha channel also linearly interpolated (dst.a = from.a * (1-t)
 *     + to.a * t). For fully-opaque sources this keeps the output
 *     opaque; partial-alpha mixes interpolate alpha the same as RGB.
 *
 * Determinism: pure IEEE-754 float32 lerp with `lroundf` for uint8
 * output, no FMA / no SIMD — same guarantee as `alpha_over`. Same
 * bytes in → same bytes out across hosts (VISION §5.3).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace me::compose {

/* Cross-dissolve `from` and `to` into `dst`:
 *   dst[i] = round(from[i] * (1 - t) + to[i] * t)   for each uint8 channel
 *
 * - All three buffers are width × height × RGBA8, tightly packed with
 *   stride_bytes between rows.
 * - t clamped to [0, 1] internally.
 * - `dst` aliasing `from` or `to` is UB (caller must provide distinct
 *   buffers; the lerp relies on reading source bytes unchanged). */
void cross_dissolve(uint8_t*       dst,
                    const uint8_t* from,
                    const uint8_t* to,
                    int            width,
                    int            height,
                    std::size_t    stride_bytes,
                    float          t);

}  // namespace me::compose
