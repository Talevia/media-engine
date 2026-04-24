#include "timeline/animated_number.hpp"
#include "timeline/animated_interp.hpp"

namespace me {

namespace {
using timeline_detail::r_lt;
using timeline_detail::fraction;
using timeline_detail::bezier_y_at_x;
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
