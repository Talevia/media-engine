/*
 * me::compose::cross_dissolve impl. See cross_dissolve.hpp for contract.
 */
#include "compose/cross_dissolve.hpp"

#include <cmath>

namespace me::compose {

namespace {

inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline uint8_t lerp_u8(uint8_t a, uint8_t b, float t) {
    const float v = static_cast<float>(a) * (1.0f - t) + static_cast<float>(b) * t;
    return static_cast<uint8_t>(std::lroundf(v));
}

}  // namespace

void cross_dissolve(uint8_t*       dst,
                    const uint8_t* from,
                    const uint8_t* to,
                    int            width,
                    int            height,
                    std::size_t    stride_bytes,
                    float          t) {
    const float t_c = clamp01(t);

    for (int y = 0; y < height; ++y) {
        uint8_t*       dr = dst  + static_cast<std::size_t>(y) * stride_bytes;
        const uint8_t* fr = from + static_cast<std::size_t>(y) * stride_bytes;
        const uint8_t* tr = to   + static_cast<std::size_t>(y) * stride_bytes;

        for (int x = 0; x < width; ++x) {
            const std::size_t p = static_cast<std::size_t>(x) * 4u;
            dr[p + 0] = lerp_u8(fr[p + 0], tr[p + 0], t_c);   /* R */
            dr[p + 1] = lerp_u8(fr[p + 1], tr[p + 1], t_c);   /* G */
            dr[p + 2] = lerp_u8(fr[p + 2], tr[p + 2], t_c);   /* B */
            dr[p + 3] = lerp_u8(fr[p + 3], tr[p + 3], t_c);   /* A */
        }
    }
}

}  // namespace me::compose
