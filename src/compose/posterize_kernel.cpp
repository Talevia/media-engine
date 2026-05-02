/* posterize_kernel impl. See header. */
#include "compose/posterize_kernel.hpp"

#include <array>
#include <cstdint>

namespace me::compose {

me_status_t apply_posterize_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::PosterizeEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (params.levels < 2 || params.levels > 256) return ME_E_INVALID_ARG;

    if (params.levels == 256) return ME_OK;  /* identity */

    /* Precompute the LUT once per call. 256 entries; per-channel
     * lookup is a single byte indirect inside the per-pixel loop. */
    const int L = params.levels;
    const int Lm1 = L - 1;
    std::array<std::uint8_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        const int bucket = (i * Lm1 + 127) / 255;          /* [0, L-1] */
        const int out    = (bucket * 255 + Lm1 / 2) / Lm1; /* [0, 255] */
        lut[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(out);
    }

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* pix = row + static_cast<std::size_t>(x) * 4;
            pix[0] = lut[pix[0]];
            pix[1] = lut[pix[1]];
            pix[2] = lut[pix[2]];
            /* Alpha (pix[3]) unmodified. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
