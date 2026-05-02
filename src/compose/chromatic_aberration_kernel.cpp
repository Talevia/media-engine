/* chromatic_aberration_kernel impl. See header.
 *
 * Snapshot the input row-by-row into a scratch buffer before
 * writing back. Without the snapshot, shifted reads within a
 * single row would pick up already-rewritten pixels and
 * produce wrong results. The full-frame snapshot is the
 * simplest correct approach for arbitrary 2D shifts (a
 * per-row buffer alone wouldn't cover Y-shifts).
 */
#include "compose/chromatic_aberration_kernel.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace me::compose {

namespace {

int clamp_coord(int v, int lo_inclusive, int hi_exclusive) {
    if (v < lo_inclusive)     return lo_inclusive;
    if (v >= hi_exclusive)    return hi_exclusive - 1;
    return v;
}

}  // namespace

me_status_t apply_chromatic_aberration_inplace(
    std::uint8_t*                              rgba,
    int                                        width,
    int                                        height,
    std::size_t                                stride_bytes,
    const me::ChromaticAberrationEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    constexpr int kMaxShift = 32;
    if (params.red_dx  < -kMaxShift || params.red_dx  > kMaxShift) return ME_E_INVALID_ARG;
    if (params.red_dy  < -kMaxShift || params.red_dy  > kMaxShift) return ME_E_INVALID_ARG;
    if (params.blue_dx < -kMaxShift || params.blue_dx > kMaxShift) return ME_E_INVALID_ARG;
    if (params.blue_dy < -kMaxShift || params.blue_dy > kMaxShift) return ME_E_INVALID_ARG;

    if (params.red_dx == 0 && params.red_dy == 0 &&
        params.blue_dx == 0 && params.blue_dy == 0) {
        return ME_OK;
    }

    /* Full-frame snapshot for non-destructive 2D shifted reads. */
    std::vector<std::uint8_t> src(stride_bytes * static_cast<std::size_t>(height));
    std::memcpy(src.data(), rgba, src.size());

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        const int ry = clamp_coord(y + params.red_dy,  0, height);
        const int by = clamp_coord(y + params.blue_dy, 0, height);
        const std::uint8_t* row_r = src.data() + static_cast<std::size_t>(ry) * stride_bytes;
        const std::uint8_t* row_g = src.data() + static_cast<std::size_t>(y)  * stride_bytes;
        const std::uint8_t* row_b = src.data() + static_cast<std::size_t>(by) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const int rx = clamp_coord(x + params.red_dx,  0, width);
            const int bx = clamp_coord(x + params.blue_dx, 0, width);
            std::uint8_t* dst = row + static_cast<std::size_t>(x) * 4;
            dst[0] = row_r[static_cast<std::size_t>(rx) * 4 + 0];
            dst[1] = row_g[static_cast<std::size_t>(x)  * 4 + 1];
            dst[2] = row_b[static_cast<std::size_t>(bx) * 4 + 2];
            dst[3] = row_g[static_cast<std::size_t>(x)  * 4 + 3];
        }
    }
    return ME_OK;
}

}  // namespace me::compose
