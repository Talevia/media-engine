#include "timeline/animated_number.hpp"

#include <algorithm>
#include <cmath>

namespace me {

namespace {

/* Rational compare: a < b <=> a.num * b.den < b.num * a.den (both
 * denominators positive post-loader validation). */
inline bool r_lt(me_rational_t a, me_rational_t b) {
    return a.num * b.den < b.num * a.den;
}

/* (t - a) / (b - a) as a double fraction in [0, 1]. Assumes
 * a.t < b.t strictly (no duplicate kf times — loader invariant). */
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
 * (0,0), (x1,y1), (x2,y2), (1,1). Uses Newton-Raphson on the
 * parameter s to invert x(s) = target_x, then returns y(s).
 *
 * x(s) = 3(1-s)²s·x1 + 3(1-s)s²·x2 + s³
 * y(s) = 3(1-s)²s·y1 + 3(1-s)s²·y2 + s³
 *
 * 8 iterations converge tightly for well-formed control points
 * (x1,x2 ∈ [0,1]). Bisection fallback unused — CSS-style beziers
 * with sane cp don't get pathological. */
double bezier_y_at_x(double target_x, double x1, double y1,
                     double x2, double y2) {
    /* Clamp x1/x2 to [0, 1] to guarantee monotonic x(s). */
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

    double s = target_x;   /* good initial guess */
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

}  // namespace

double AnimatedNumber::evaluate_at(me_rational_t t) const {
    if (static_value.has_value()) return *static_value;
    if (keyframes.empty()) return 0.0;

    /* Before first / single kf → first.v. */
    if (r_lt(t, keyframes.front().t) || keyframes.size() == 1) {
        return keyframes.front().v;
    }
    /* After last → last.v. */
    if (!r_lt(t, keyframes.back().t)) {
        return keyframes.back().v;
    }

    /* Find bracketing pair [a, b] with a.t <= t < b.t. Linear
     * scan — kf counts per property are small (typically < 20). */
    std::size_t ai = 0;
    for (std::size_t i = 1; i < keyframes.size(); ++i) {
        if (r_lt(t, keyframes[i].t)) { ai = i - 1; break; }
    }
    const Keyframe& a = keyframes[ai];
    const Keyframe& b = keyframes[ai + 1];

    switch (a.interp) {
    case Interp::Hold:
    case Interp::Stepped:
        return a.v;
    case Interp::Linear: {
        const double u = fraction(t, a.t, b.t);
        return a.v + u * (b.v - a.v);
    }
    case Interp::Bezier: {
        const double u = fraction(t, a.t, b.t);
        const double bez_y = bezier_y_at_x(u, a.cp[0], a.cp[1], a.cp[2], a.cp[3]);
        return a.v + bez_y * (b.v - a.v);
    }
    }
    return a.v;   /* unreachable; defensive */
}

}  // namespace me
