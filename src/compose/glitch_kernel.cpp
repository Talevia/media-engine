/* glitch_kernel impl. See header for the contract.
 *
 * Allocates a per-row source-byte vector and writes back into
 * the input buffer (rgba) row-by-row. The per-row scratch
 * buffer is necessary because the shift is destructive — if
 * we wrote shifted bytes directly into rgba, subsequent reads
 * within the same row would pick up already-shifted data.
 */
#include "compose/glitch_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace me::compose {

namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime  = 0x100000001b3ULL;

std::uint64_t mix_block(std::uint64_t seed, std::uint64_t by) {
    std::uint64_t h = kFnvOffset;
    h ^= seed; h *= kFnvPrime;
    h ^= by;   h *= kFnvPrime;
    return h;
}

std::uint64_t xorshift64(std::uint64_t s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

int clamp_x(int x, int width) {
    if (x < 0)        return 0;
    if (x >= width)   return width - 1;
    return x;
}

}  // namespace

me_status_t apply_glitch_inplace(
    std::uint8_t*                 rgba,
    int                           width,
    int                           height,
    std::size_t                   stride_bytes,
    const me::GlitchEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (!std::isfinite(params.intensity)) return ME_E_INVALID_ARG;
    if (params.block_size_px < 1 || params.block_size_px > 64) return ME_E_INVALID_ARG;
    if (params.channel_shift_max_px < 0 || params.channel_shift_max_px > 16) return ME_E_INVALID_ARG;

    const float intensity = std::max(params.intensity, 0.0f);
    if (intensity == 0.0f) return ME_OK;

    const int max_shift     = static_cast<int>(intensity * static_cast<float>(width) + 0.5f);
    if (max_shift == 0 && params.channel_shift_max_px == 0) return ME_OK;

    const int shift_range = 2 * max_shift + 1;
    const int chshift_range = 2 * params.channel_shift_max_px + 1;
    const int gs = params.block_size_px;

    /* Per-row scratch buffer: copy of the input row, used as
     * the source for shifted reads. Allocated once outside
     * the loop. */
    std::vector<std::uint8_t> src_row(static_cast<std::size_t>(width) * 4);

    for (int y = 0; y < height; ++y) {
        const int by = y / gs;
        const std::uint64_t h0 = mix_block(params.seed,
                                            static_cast<std::uint64_t>(by));
        const std::uint64_t r1 = xorshift64(h0);
        const int block_shift = (max_shift > 0)
            ? (static_cast<int>(r1 % static_cast<std::uint64_t>(shift_range))
               - max_shift)
            : 0;

        const std::uint64_t r2 = xorshift64(r1);
        const int r_shift = (params.channel_shift_max_px > 0)
            ? (static_cast<int>(r2 % static_cast<std::uint64_t>(chshift_range))
               - params.channel_shift_max_px)
            : 0;
        const std::uint64_t r3 = xorshift64(r2);
        const int b_shift = (params.channel_shift_max_px > 0)
            ? (static_cast<int>(r3 % static_cast<std::uint64_t>(chshift_range))
               - params.channel_shift_max_px)
            : 0;

        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        std::memcpy(src_row.data(), row,
                     static_cast<std::size_t>(width) * 4);

        for (int x = 0; x < width; ++x) {
            const int srcx_g = clamp_x(x - block_shift, width);
            const int srcx_r = clamp_x(srcx_g + r_shift, width);
            const int srcx_b = clamp_x(srcx_g + b_shift, width);

            std::uint8_t* dst = row + static_cast<std::size_t>(x) * 4;
            dst[0] = src_row[static_cast<std::size_t>(srcx_r) * 4 + 0];
            dst[1] = src_row[static_cast<std::size_t>(srcx_g) * 4 + 1];
            dst[2] = src_row[static_cast<std::size_t>(srcx_b) * 4 + 2];
            /* Alpha sourced from the G-shifted (block-shifted)
             * column for consistency; unchanged when
             * block_shift == 0. */
            dst[3] = src_row[static_cast<std::size_t>(srcx_g) * 4 + 3];
        }
    }
    return ME_OK;
}

}  // namespace me::compose
