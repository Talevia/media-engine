/* `EffectKind::ToneCurve` typed parameters — M12 §155 first-of-
 * four color effects.
 *
 * Per-channel tone curve. Each curve is a list of `{x, y}`
 * control points (uint8 input → uint8 output); the kernel
 * precomputes a 256-byte LUT per channel via piecewise-linear
 * interpolation between adjacent control points (clamp at
 * endpoints). The default-constructed param is identity (no
 * control points → LUT[i] = i for every channel).
 *
 * Three optional curves: `r`, `g`, `b` (per-RGB-channel
 * control). When a per-channel curve is empty, the channel is
 * left unmodified. The luminance curve `luma` is reserved for
 * a future cycle once a luma → channel projection is wired
 * (today only the per-channel curves apply).
 *
 * Determinism: precomputed LUTs + integer index lookup; no
 * float in the byte-output path. VISION §3.1 byte-identity
 * holds. */
#pragma once

#include <cstdint>
#include <vector>

namespace me {

/* One control point on a tone curve. `x` and `y` are uint8
 * input/output; the kernel interpolates between adjacent
 * points. Curve must be sorted by `x`. */
struct ToneCurvePoint {
    std::uint8_t x;
    std::uint8_t y;
};

struct ToneCurveEffectParams {
    /* Per-channel curves. Empty curve = channel unchanged
     * (LUT[i] = i). When non-empty, MUST contain at least 2
     * points; the loader enforces this. Points must be sorted
     * by `x`. */
    std::vector<ToneCurvePoint> r;
    std::vector<ToneCurvePoint> g;
    std::vector<ToneCurvePoint> b;
};

}  // namespace me
