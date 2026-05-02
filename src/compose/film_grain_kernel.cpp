/* film_grain_kernel impl. See header for the contract.
 *
 * Mixing function: combines `(seed, block_y, block_x)` into a
 * single uint64 hash via FNV-1a-style xor-mult, then runs
 * xorshift64 on the result to produce the actual noise value.
 * The mixing prevents row-aligned correlation (block_y alone
 * would correlate adjacent rows; mixing with block_x and seed
 * produces a stippled pattern).
 *
 * Same `(seed, block_y, block_x)` → same hash → same xorshift
 * sequence → same delta byte across hosts. Pure integer math.
 */
#include "compose/film_grain_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace me::compose {

namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime  = 0x100000001b3ULL;

/* Mix three uint64s into one via FNV-1a. */
std::uint64_t mix_seed(std::uint64_t seed,
                        std::uint64_t bx,
                        std::uint64_t by) {
    std::uint64_t h = kFnvOffset;
    h ^= seed; h *= kFnvPrime;
    h ^= by;   h *= kFnvPrime;
    h ^= bx;   h *= kFnvPrime;
    return h;
}

/* xorshift64 — Marsaglia's variant with shifts (13, 7, 17). */
std::uint64_t xorshift64(std::uint64_t s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

std::uint8_t add_clamp_byte(std::uint8_t v, int delta) {
    int r = static_cast<int>(v) + delta;
    if (r < 0)   return 0;
    if (r > 255) return 255;
    return static_cast<std::uint8_t>(r);
}

}  // namespace

me_status_t apply_film_grain_inplace(
    std::uint8_t*                      rgba,
    int                                width,
    int                                height,
    std::size_t                        stride_bytes,
    const me::FilmGrainEffectParams&   params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (!std::isfinite(params.amount)) return ME_E_INVALID_ARG;
    if (params.grain_size_px < 1 || params.grain_size_px > 8) return ME_E_INVALID_ARG;

    const float amount = std::max(params.amount, 0.0f);
    if (amount == 0.0f) return ME_OK;

    /* Compute the absolute max delta as int(amount * 127). The
     * xorshift output is reduced modulo (2 * max_delta + 1)
     * and shifted to [-max_delta, +max_delta]. amount=1 gives
     * full ±127 range; typical grain settings of 0.05..0.2
     * give ±6..±25. */
    const int max_delta = static_cast<int>(amount * 127.0f + 0.5f);
    if (max_delta == 0) return ME_OK;
    const int range = 2 * max_delta + 1;

    const int gs = params.grain_size_px;

    for (int y = 0; y < height; ++y) {
        const int by = y / gs;
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const int bx = x / gs;
            const std::uint64_t seed = mix_seed(params.seed,
                                                  static_cast<std::uint64_t>(bx),
                                                  static_cast<std::uint64_t>(by));
            const std::uint64_t r = xorshift64(seed);
            /* Map xorshift output to [-max_delta, +max_delta]. */
            const int delta = static_cast<int>(r % static_cast<std::uint64_t>(range))
                              - max_delta;

            std::uint8_t* pix = row + static_cast<std::size_t>(x) * 4;
            pix[0] = add_clamp_byte(pix[0], delta);
            pix[1] = add_clamp_byte(pix[1], delta);
            pix[2] = add_clamp_byte(pix[2], delta);
            /* Alpha (pix[3]) unmodified. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
