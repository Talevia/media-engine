/*
 * Shared interpolation primitives for me::AnimatedNumber +
 * me::AnimatedColor.
 *
 * Both animated-property types bracket keyframes by rational
 * time and apply the same set of `Interp` modes (linear / bezier
 * / hold / stepped). The math is identical — only the per-channel
 * value type differs (double vs uint8_t[4]). This header hosts the
 * two shared helpers so one owner holds the reference
 * implementation:
 *
 *   fraction(t, a, b) — returns u ∈ [0, 1] for rational t in
 *     the half-open interval [a, b). Clamps out-of-range.
 *   bezier_y_at_x(x, x1, y1, x2, y2) — CSS cubic-bezier y(x)
 *     via Newton-Raphson on the parameter s. Clamps cp to
 *     [0, 1] to guarantee monotonic x(s).
 *
 * Internal-only. Not exposed via any public header. Both helpers
 * are defined `inline` so each TU gets a local copy — no .cpp
 * needed, no link-order concerns.
 */
#pragma once

#include "media_engine/types.h"

#include <cmath>

namespace me::timeline_detail {

/* Rational less-than: a < b ⇔ a.num * b.den < b.num * a.den.
 * Denominators assumed positive (loader invariant). */
inline bool r_lt(me_rational_t a, me_rational_t b) {
    return a.num * b.den < b.num * a.den;
}

/* (t - a) / (b - a) as a double in [0, 1]. Requires a.t < b.t
 * strictly (no duplicate kf times — loader invariant). Out-of-
 * range inputs get clamped. */
inline double fraction(me_rational_t t, me_rational_t a, me_rational_t b) {
    const double num = static_cast<double>(t.num) * b.den * a.den -
                        static_cast<double>(a.num) * t.den * b.den;
    const double den = static_cast<double>(b.num) * a.den * t.den -
                        static_cast<double>(a.num) * b.den * t.den;
    if (den == 0.0) return 0.0;
    double u = num / den;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    return u;
}

/* Evaluate CSS cubic-bezier y(x=target_x) with control points
 * (0,0), (x1,y1), (x2,y2), (1,1). Newton-Raphson on the
 * parameter s to invert x(s) = target_x, then returns y(s).
 *
 *   x(s) = 3(1-s)²s·x1 + 3(1-s)s²·x2 + s³
 *   y(s) = 3(1-s)²s·y1 + 3(1-s)s²·y2 + s³
 *
 * 8 iterations converge tightly for well-formed control points
 * (x1, x2 ∈ [0, 1]). Bisection fallback unused — CSS-style
 * beziers with sane cp don't get pathological. */
inline double bezier_y_at_x(double target_x, double x1, double y1,
                              double x2, double y2) {
    if (x1 < 0.0) x1 = 0.0;
    if (x1 > 1.0) x1 = 1.0;
    if (x2 < 0.0) x2 = 0.0;
    if (x2 > 1.0) x2 = 1.0;
    if (target_x <= 0.0) return 0.0;
    if (target_x >= 1.0) return 1.0;

    auto x_at = [&](double s) {
        const double inv = 1.0 - s;
        return 3.0 * inv * inv * s * x1 + 3.0 * inv * s * s * x2 + s * s * s;
    };
    auto dx_at = [&](double s) {
        const double inv = 1.0 - s;
        return 3.0 * inv * inv * x1 +
               6.0 * inv * s * (x2 - x1) +
               3.0 * s * s * (1.0 - x2);
    };
    auto y_at = [&](double s) {
        const double inv = 1.0 - s;
        return 3.0 * inv * inv * s * y1 + 3.0 * inv * s * s * y2 + s * s * s;
    };

    double s = target_x;
    for (int i = 0; i < 8; ++i) {
        const double cur = x_at(s) - target_x;
        if (std::fabs(cur) < 1e-6) break;
        const double deriv = dx_at(s);
        if (std::fabs(deriv) < 1e-12) break;
        s -= cur / deriv;
        if (s < 0.0) s = 0.0;
        if (s > 1.0) s = 1.0;
    }
    return y_at(s);
}

}  // namespace me::timeline_detail
