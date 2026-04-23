/*
 * me::compose::alpha_over — pixel-level composite kernel for multi-track
 * video compose.
 *
 * Lands as part of the `multi-track-compose-kernel` cycle's first slice.
 * The full M2 exit criterion ("2+ video tracks 叠加, alpha / blend mode
 * 正确") is split across several cycles; this file provides the pure
 * kernel function that future sink-layer work (multi-track-compose-
 * sink-wire) consumes to actually feed the encoder with composited
 * frames.
 *
 * Shape:
 *   - Input buffers are RGBA8 (non-premultiplied alpha), row-major,
 *     tightly packed (stride == width * 4).
 *   - Kernel composites `src` over `dst` **in-place on `dst`**, scaling
 *     src's alpha by `opacity` ∈ [0, 1].
 *   - Three blend modes: Normal (Porter-Duff source-over), Multiply
 *     (channel multiply), Screen (inverse multiply). Covers the M2
 *     direction line "normal/multiply/screen". Additional modes land
 *     in later milestones when the use case surfaces.
 *
 * Determinism: pure IEEE-754 float32 math, round-to-nearest via
 * lroundf. No FMA / no -ffast-math / no SIMD path (yet). Same bytes
 * in → same bytes out across all supported hosts (VISION §5.3).
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace me::compose {

enum class BlendMode : uint8_t {
    Normal   = 0,  /* src-over (Porter-Duff) */
    Multiply = 1,  /* per-channel src * dst, then src-over */
    Screen   = 2,  /* 1 - (1-src) * (1-dst), then src-over */
};

/* Composite `src` over `dst` in-place on `dst`. Both buffers are
 * width * height RGBA8 (non-premultiplied), stride in bytes.
 * `opacity` clamps to [0, 1] internally.
 *
 * Returns void — there is no failure mode for valid inputs (pure
 * byte math). Caller responsible for pointer non-null and buffer
 * size correctness; undefined behavior on null / out-of-bounds. */
void alpha_over(uint8_t*       dst,
                const uint8_t* src,
                int            width,
                int            height,
                std::size_t    stride_bytes,
                float          opacity,
                BlendMode      mode);

}  // namespace me::compose
