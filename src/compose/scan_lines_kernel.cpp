/* scan_lines_kernel impl. See header for the contract.
 *
 * Multiplier = round(255 * (1 - darkness)) clamped to
 * [0, 255]. Per-pixel: out = round(in * multiplier / 255).
 * Round-half-up integer divide via `(numer + 127) / 255`.
 */
#include "compose/scan_lines_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace me::compose {

me_status_t apply_scan_lines_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::ScanLinesEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (!std::isfinite(params.darkness)) return ME_E_INVALID_ARG;
    if (params.line_height_px < 1 || params.line_height_px > 64) return ME_E_INVALID_ARG;
    if (params.phase_offset_px < 0 || params.phase_offset_px >= 64) return ME_E_INVALID_ARG;

    const float darkness = std::clamp(params.darkness, 0.0f, 1.0f);
    if (darkness == 0.0f) return ME_OK;

    /* Multiplier in [0, 255]. round-half-up via the standard
     * float-to-byte convention (we accept the float→int
     * cast at parameter time; the per-pixel loop is pure
     * integer). */
    const int mult = static_cast<int>((1.0f - darkness) * 255.0f + 0.5f);

    const int line_h    = params.line_height_px;
    const int phase_mod = params.phase_offset_px % line_h;

    for (int y = 0; y < height; ++y) {
        if ((y % line_h) != phase_mod) continue;
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* pix = row + static_cast<std::size_t>(x) * 4;
            /* round-half-up: (in * mult + 127) / 255 */
            pix[0] = static_cast<std::uint8_t>(
                (static_cast<int>(pix[0]) * mult + 127) / 255);
            pix[1] = static_cast<std::uint8_t>(
                (static_cast<int>(pix[1]) * mult + 127) / 255);
            pix[2] = static_cast<std::uint8_t>(
                (static_cast<int>(pix[2]) * mult + 127) / 255);
            /* Alpha (pix[3]) unmodified. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
