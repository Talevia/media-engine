#include "timeline/animated_color.hpp"

#include <cmath>

namespace me {

namespace {

inline bool r_lt(me_rational_t a, me_rational_t b) {
    return a.num * b.den < b.num * a.den;
}

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

/* Duplicate of animated_number.cpp's bezier_y_at_x — we don't
 * share because lifting it to a public helper would force a
 * stable-ABI decision about a math routine. Two call sites for
 * now; extract only if a third lands. */
double bezier_y_at_x(double target_x, double x1, double y1,
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

inline std::uint8_t lerp_u8(std::uint8_t a, std::uint8_t b, double u) {
    const double v = static_cast<double>(a) + u * (static_cast<double>(b) - a);
    const int r = static_cast<int>(std::round(v));
    if (r < 0)   return 0;
    if (r > 255) return 255;
    return static_cast<std::uint8_t>(r);
}

}  // namespace

std::array<std::uint8_t, 4> AnimatedColor::evaluate_at(me_rational_t t) const {
    if (static_value.has_value()) return *static_value;
    if (keyframes.empty()) return {0xFF, 0xFF, 0xFF, 0xFF};

    if (r_lt(t, keyframes.front().t) || keyframes.size() == 1) {
        return keyframes.front().v;
    }
    if (!r_lt(t, keyframes.back().t)) {
        return keyframes.back().v;
    }

    std::size_t ai = 0;
    for (std::size_t i = 1; i < keyframes.size(); ++i) {
        if (r_lt(t, keyframes[i].t)) { ai = i - 1; break; }
    }
    const ColorKeyframe& a = keyframes[ai];
    const ColorKeyframe& b = keyframes[ai + 1];

    switch (a.interp) {
    case Interp::Hold:
    case Interp::Stepped:
        return a.v;
    case Interp::Linear: {
        const double u = fraction(t, a.t, b.t);
        return {
            lerp_u8(a.v[0], b.v[0], u),
            lerp_u8(a.v[1], b.v[1], u),
            lerp_u8(a.v[2], b.v[2], u),
            lerp_u8(a.v[3], b.v[3], u),
        };
    }
    case Interp::Bezier: {
        const double u   = fraction(t, a.t, b.t);
        const double bez = bezier_y_at_x(u, a.cp[0], a.cp[1], a.cp[2], a.cp[3]);
        return {
            lerp_u8(a.v[0], b.v[0], bez),
            lerp_u8(a.v[1], b.v[1], bez),
            lerp_u8(a.v[2], b.v[2], bez),
            lerp_u8(a.v[3], b.v[3], bez),
        };
    }
    }
    return a.v;
}

}  // namespace me
