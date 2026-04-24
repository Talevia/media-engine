#include "timeline/animated_color.hpp"
#include "timeline/animated_interp.hpp"

#include <cmath>
#include <string>

namespace me {

namespace {

using timeline_detail::r_lt;
using timeline_detail::fraction;
using timeline_detail::bezier_y_at_x;

inline std::uint8_t lerp_u8(std::uint8_t a, std::uint8_t b, double u) {
    const double v = static_cast<double>(a) + u * (static_cast<double>(b) - a);
    const int r = static_cast<int>(std::round(v));
    if (r < 0)   return 0;
    if (r > 255) return 255;
    return static_cast<std::uint8_t>(r);
}

}  // namespace

namespace {

std::uint8_t nibble_from(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    return 0;
}
std::uint8_t byte_from(char hi, char lo) {
    return static_cast<std::uint8_t>((nibble_from(hi) << 4) | nibble_from(lo));
}

}  // namespace

AnimatedColor AnimatedColor::from_hex(const char* hex) {
    if (!hex) return default_opaque_white();
    const std::size_t n = std::char_traits<char>::length(hex);
    if ((n != 7 && n != 9) || hex[0] != '#') {
        return default_opaque_white();
    }
    std::array<std::uint8_t, 4> rgba{0xFF, 0xFF, 0xFF, 0xFF};
    rgba[0] = byte_from(hex[1], hex[2]);
    rgba[1] = byte_from(hex[3], hex[4]);
    rgba[2] = byte_from(hex[5], hex[6]);
    if (n == 9) rgba[3] = byte_from(hex[7], hex[8]);
    return from_static(rgba);
}

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
