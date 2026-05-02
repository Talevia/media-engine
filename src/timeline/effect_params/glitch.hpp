/* `EffectKind::Glitch` typed parameters — M12 §156
 * (1/5 stylized effects).
 *
 * "Digital corruption" effect: horizontal block stripes shift
 * left/right by a deterministic per-row amount, with optional
 * per-channel (R/B) shift on top. Same seed + same frame
 * coords → same shift bytes across hosts (xorshift64 PRNG
 * keyed on `(seed, block_y)`).
 *
 * Default-constructed param is identity (intensity=0); the
 * kernel recognizes this and skips the per-pixel transform. */
#pragma once

#include <cstdint>

namespace me {

struct GlitchEffectParams {
    /* PRNG seed — REQUIRED for VISION §3.1 byte-identity.
     * Same seed → same shift pattern across hosts. Default 0
     * = no glitch (identity); host should pick a fixed seed
     * per shot for shot-stable distortion. */
    std::uint64_t seed = 0;

    /* Magnitude of the horizontal block shift, normalized to
     * fraction of frame width. 0.0 = no effect, 1.0 = up to
     * full-width shift (extreme). Typical glitch values
     * 0.05..0.2. Negative clamps to 0. */
    float intensity = 0.0f;

    /* Block height in pixels (rows shifted as a unit).
     * Smaller = stripier glitch; larger = chunkier. Range
     * [1, 64]. */
    int block_size_px = 8;

    /* Maximum per-channel horizontal shift on top of the
     * block shift. R reads from `(shifted_x + R_shift, y)`;
     * B from `(shifted_x + B_shift, y)`; G from
     * `shifted_x`. Range [0, 16] px. Set to 0 for pure
     * block-shift (no chromatic aberration look). */
    int channel_shift_max_px = 0;
};

}  // namespace me
