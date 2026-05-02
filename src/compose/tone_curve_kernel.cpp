/* tone_curve_kernel impl. See header for the contract.
 *
 * LUT build: for each non-empty channel curve, walk the
 * control points pairwise. For every uint8 input value `i`,
 * find the point pair (a, b) such that a.x ≤ i ≤ b.x, then
 * compute LUT[i] = a.y + (b.y - a.y) * (i - a.x) / (b.x - a.x).
 * The /(b.x - a.x) divide uses round-half-up integer math
 * (`(numer + (denom / 2)) / denom`) so two hosts produce the
 * same LUT bytes for the same control points.
 *
 * Out-of-range input values clamp at the endpoint y values.
 */
#include "compose/tone_curve_kernel.hpp"

#include <array>
#include <cstdint>

namespace me::compose {

namespace {

using Lut = std::array<std::uint8_t, 256>;

/* Build a LUT from a non-empty control-point list. Returns an
 * identity LUT (LUT[i] = i) for an empty point list — caller
 * uses this to skip per-channel work cheaply.
 *
 * Precondition: `points.size() != 1` (1-point curves are
 * rejected at the kernel boundary). Caller passes the
 * already-validated list. */
Lut build_lut(const std::vector<me::ToneCurvePoint>& points) {
    Lut lut{};
    if (points.empty()) {
        for (int i = 0; i < 256; ++i) lut[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(i);
        return lut;
    }

    /* Clamp the leading region to points[0].y. */
    const auto& first = points.front();
    for (int i = 0; i < first.x; ++i) {
        lut[static_cast<std::size_t>(i)] = first.y;
    }

    /* Walk segment pairs. Each segment is [points[k].x, points[k+1].x]. */
    for (std::size_t k = 0; k + 1 < points.size(); ++k) {
        const auto& a = points[k];
        const auto& b = points[k + 1];
        const int x0 = a.x;
        const int x1 = b.x;
        const int y0 = a.y;
        const int y1 = b.y;
        const int dx = x1 - x0;
        if (dx <= 0) {
            /* Degenerate segment (overlapping or out-of-order
             * points) — the loader rejects sorted-violation but
             * a duplicate `x` is technically valid. Preserve the
             * later `y` so user intent of "step at this x" is
             * the documented outcome. */
            lut[static_cast<std::size_t>(x0)] = static_cast<std::uint8_t>(y1);
            continue;
        }
        const int dy = y1 - y0;
        const int half = dx / 2;
        for (int x = x0; x <= x1; ++x) {
            const int numer = (x - x0) * dy;
            /* Round-half-up integer divide (preserve sign for
             * negative dy via standard divide; (numer + half)
             * works because half is non-negative and dx > 0). */
            const int q = (numer >= 0) ? ((numer + half) / dx)
                                       : (-((-numer + half) / dx));
            int y = y0 + q;
            if (y < 0)   y = 0;
            if (y > 255) y = 255;
            lut[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(y);
        }
    }

    /* Clamp the trailing region to points.back().y. */
    const auto& last = points.back();
    for (int i = last.x + 1; i < 256; ++i) {
        lut[static_cast<std::size_t>(i)] = last.y;
    }
    return lut;
}

}  // namespace

me_status_t apply_tone_curve_inplace(
    std::uint8_t*                    rgba,
    int                              width,
    int                              height,
    std::size_t                      stride_bytes,
    const me::ToneCurveEffectParams& params) {
    if (!rgba || width <= 0 || height <= 0) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;

    /* Reject 1-point curves defensively. The loader also
     * enforces this; the kernel's check guards against a
     * direct test-only construction with a malformed param. */
    if (params.r.size() == 1) return ME_E_INVALID_ARG;
    if (params.g.size() == 1) return ME_E_INVALID_ARG;
    if (params.b.size() == 1) return ME_E_INVALID_ARG;

    /* Skip work entirely if all three curves are empty. */
    if (params.r.empty() && params.g.empty() && params.b.empty()) return ME_OK;

    const Lut lut_r = build_lut(params.r);
    const Lut lut_g = build_lut(params.g);
    const Lut lut_b = build_lut(params.b);

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = rgba + static_cast<std::size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            px[0] = lut_r[px[0]];
            px[1] = lut_g[px[1]];
            px[2] = lut_b[px[2]];
            /* Alpha (px[3]) unmodified. */
        }
    }
    return ME_OK;
}

}  // namespace me::compose
