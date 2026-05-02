/* `EffectKind::OrderedDither` typed parameters — M12 §156
 * (5/5 stylized effects).
 *
 * Bayer-matrix ordered dither + per-channel quantization.
 * No PRNG (Bayer is the deterministic alternative to seeded
 * blue noise). The kernel adds a Bayer threshold offset to
 * each channel before quantizing to N levels — at quantum
 * boundaries the offset breaks the otherwise-flat banding
 * into a periodic stipple pattern.
 *
 * Default-constructed param is identity (levels=256); the
 * kernel recognizes this and skips the per-pixel transform.
 *
 * Three matrix sizes are supported (Bayer 2×2, 4×4, 8×8) —
 * larger matrices = finer dither pattern. */
#pragma once

#include <cstdint>

namespace me {

struct OrderedDitherEffectParams {
    /* Bayer matrix size. 2 = 2×2 (4 cells), 4 = 4×4 (16
     * cells), 8 = 8×8 (64 cells). Other values rejected. */
    int matrix_size = 4;

    /* Quantization levels per channel after dither. Range
     * [2, 256]. levels=256 = identity (kernel skip). */
    int levels      = 256;
};

}  // namespace me
