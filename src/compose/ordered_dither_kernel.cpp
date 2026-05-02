/* ordered_dither_kernel impl. See header.
 *
 * Three canonical Bayer matrices: 2×2, 4×4, 8×8. Values
 * stored as integers in [0, N²-1]. (Top-left submatrix of
 * the 8×8 isn't directly the smaller Bayer matrix — values
 * are scaled by 64/N² — so we hard-code the three sizes
 * separately for clarity.) */
#include "compose/ordered_dither_kernel.hpp"

#include <array>
#include <cstdint>

namespace me::compose {

namespace {

constexpr std::array<std::uint8_t, 4> kBayer2 = {
    0, 2,
    3, 1,
};

constexpr std::array<std::uint8_t, 16> kBayer4 = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

constexpr std::array<std::uint8_t, 64> kBayer8 = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
};

inline std::uint8_t bayer_value(int N, int my, int mx) {
    if (N == 2) return kBayer2[static_cast<std::size_t>(my) * 2 + mx];
    if (N == 4) return kBayer4[static_cast<std::size_t>(my) * 4 + mx];
    return kBayer8[static_cast<std::size_t>(my) * 8 + mx];
}

}  // namespace

me_status_t apply_ordered_dither_inplace(
    std::uint8_t*                        rgba,
    int                                  width,
    int                                  height,
    std::size_t                          stride_bytes,
    const me::OrderedDitherEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (params.matrix_size != 2 && params.matrix_size != 4 &&
        params.matrix_size != 8) {
        return ME_E_INVALID_ARG;
    }
    if (params.levels < 2 || params.levels > 256) return ME_E_INVALID_ARG;

    if (params.levels == 256) return ME_OK;  /* identity */

    const int N = params.matrix_size;
    const int N2 = N * N;       /* 4 / 16 / 64 */
    const int Lm1 = params.levels - 1;
    const int step = 255 / Lm1; /* quantum width */

    /* Map matrix value [0, N²-1] to a signed offset that
     * spreads over one quantum (-step/2 .. +step/2):
     *   offset = ((2b - (N²-1)) * step) / (2 * N²),
     * with banker-style rounding toward zero. Adding the
     * offset before quantization breaks bands into a
     * periodic stipple. */
    const int two_N2 = 2 * N2;
    const int N2m1   = N2 - 1;

    /* Precompute LUT[matrix_value][input byte] → output byte.
     * Size: up to 64 * 256 = 16 KB, fine on stack. */
    std::array<std::uint8_t, 64 * 256> lut{};
    for (int b = 0; b < N2; ++b) {
        const int numer = (2 * b - N2m1) * step;
        const int offset = (numer >= 0)
            ? ((numer + (two_N2 / 2)) / two_N2)
            : -(((-numer) + (two_N2 / 2)) / two_N2);
        for (int i = 0; i < 256; ++i) {
            int v = i + offset;
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            const int bucket = (v * Lm1 + 127) / 255;
            const int out    = (bucket * 255 + Lm1 / 2) / Lm1;
            lut[static_cast<std::size_t>(b) * 256 + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(out);
        }
    }

    for (int y = 0; y < height; ++y) {
        const int my = y % N;
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const int mx = x % N;
            const int b  = bayer_value(N, my, mx);
            std::uint8_t* pix = row + static_cast<std::size_t>(x) * 4;
            const std::size_t base = static_cast<std::size_t>(b) * 256;
            pix[0] = lut[base + pix[0]];
            pix[1] = lut[base + pix[1]];
            pix[2] = lut[base + pix[2]];
            /* Alpha (pix[3]) unmodified. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
