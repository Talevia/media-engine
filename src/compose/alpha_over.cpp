/*
 * me::compose::alpha_over impl. See alpha_over.hpp for contract.
 */
#include "compose/alpha_over.hpp"

#include <algorithm>
#include <cmath>

namespace me::compose {

namespace {

inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Convert uint8 channel value to normalised float [0, 1]. Exact for
 * 0 and 255, monotonic in between. */
inline float to_float(uint8_t v) {
    return static_cast<float>(v) * (1.0f / 255.0f);
}

/* Convert normalised float back to uint8 with deterministic round-
 * to-nearest (half-away-from-zero). `lroundf` is IEEE-754 stable
 * across hosts; `static_cast<int>(x + 0.5f)` is not (banker's
 * rounding disagreements). */
inline uint8_t to_u8(float v) {
    const float c = clamp01(v) * 255.0f;
    return static_cast<uint8_t>(std::lroundf(c));
}

/* Apply the blend-mode transform to src RGB in [0,1] space before
 * the Porter-Duff source-over composite with dst. Returns the
 * transformed src RGB triple (alpha is orthogonal). */
struct RGB { float r, g, b; };

inline RGB blend_mode_apply(BlendMode mode, RGB src, RGB dst) {
    switch (mode) {
    case BlendMode::Normal:
        return src;
    case BlendMode::Multiply:
        return { src.r * dst.r, src.g * dst.g, src.b * dst.b };
    case BlendMode::Screen:
        return { 1.0f - (1.0f - src.r) * (1.0f - dst.r),
                 1.0f - (1.0f - src.g) * (1.0f - dst.g),
                 1.0f - (1.0f - src.b) * (1.0f - dst.b) };
    }
    return src;  /* unreachable; switch covers every enumerator */
}

}  // namespace

void alpha_over(uint8_t*       dst,
                const uint8_t* src,
                int            width,
                int            height,
                std::size_t    stride_bytes,
                float          opacity,
                BlendMode      mode) {
    const float op = clamp01(opacity);

    for (int y = 0; y < height; ++y) {
        uint8_t*       dst_row =       dst + static_cast<std::size_t>(y) * stride_bytes;
        const uint8_t* src_row =       src + static_cast<std::size_t>(y) * stride_bytes;

        for (int x = 0; x < width; ++x) {
            const std::size_t p = static_cast<std::size_t>(x) * 4u;

            const RGB src_rgb{ to_float(src_row[p + 0]),
                               to_float(src_row[p + 1]),
                               to_float(src_row[p + 2]) };
            const float src_a_raw = to_float(src_row[p + 3]);
            const float src_a     = src_a_raw * op;   /* opacity scales alpha */

            const RGB dst_rgb{ to_float(dst_row[p + 0]),
                               to_float(dst_row[p + 1]),
                               to_float(dst_row[p + 2]) };
            const float dst_a = to_float(dst_row[p + 3]);

            /* Blend mode transforms src RGB against dst RGB in straight-
             * alpha space. Per-mode contract lives in alpha_over.hpp. */
            const RGB src_blended = blend_mode_apply(mode, src_rgb, dst_rgb);

            /* Porter-Duff source-over (straight alpha):
             *   out_rgb = src_blended * src_a + dst_rgb * (1 - src_a)
             *   out_a   = src_a + dst_a * (1 - src_a)
             */
            const float inv_src_a = 1.0f - src_a;
            const RGB out_rgb{
                src_blended.r * src_a + dst_rgb.r * inv_src_a,
                src_blended.g * src_a + dst_rgb.g * inv_src_a,
                src_blended.b * src_a + dst_rgb.b * inv_src_a,
            };
            const float out_a = src_a + dst_a * inv_src_a;

            dst_row[p + 0] = to_u8(out_rgb.r);
            dst_row[p + 1] = to_u8(out_rgb.g);
            dst_row[p + 2] = to_u8(out_rgb.b);
            dst_row[p + 3] = to_u8(out_a);
        }
    }
}

}  // namespace me::compose
