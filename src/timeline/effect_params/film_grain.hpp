/* `EffectKind::FilmGrain` typed parameters — M12 §155
 * (4/4 color effects).
 *
 * Per-pixel additive noise sourced from a deterministic PRNG.
 * VISION §3.1 byte-identity demands same `seed` + same frame
 * coordinate → same grain bytes across hosts. The kernel uses
 * xorshift64 seeded from `(seed, frame_y, frame_x)` so every
 * pixel's noise is reproducible without any cross-pixel
 * dependency.
 *
 * Default-constructed param is identity (amount=0); the kernel
 * recognizes this and skips the per-pixel transform. */
#pragma once

#include <cstdint>

namespace me {

struct FilmGrainEffectParams {
    /* PRNG seed (REQUIRED for cross-host byte-identity). The
     * kernel mixes this with the per-pixel coordinate via
     * xorshift64 so the whole frame is reproducible. Default
     * 0 = no grain (identity); any non-zero seed engages the
     * per-pixel pass. The host should pick a fixed seed per
     * shot for shot-stable grain across re-renders. */
    std::uint64_t seed = 0;

    /* Grain magnitude. 0.0 = no effect, 1.0 = ±127 LSB
     * variation (full-range additive noise — extreme; typical
     * cinema-grain values are 0.05..0.2). Negative clamps to
     * 0; >1 saturates with byte clamping at the output. */
    float amount = 0.0f;

    /* Grain block size in pixels. 1 = per-pixel noise (sharp
     * graininess); larger values = chunkier blocks (low-res
     * grain). Range [1, 8]. Out-of-range values are clamped
     * by the loader. */
    int   grain_size_px = 1;
};

}  // namespace me
