/*
 * me::AnimatedNumber — value-over-time primitive matching the
 * schema's `{"static": v}` / `{"keyframes": [...]}` pair.
 *
 * Scope-A slice of `transform-animated-support` (M3 exit criterion
 * "所有 animated property 类型的插值正确"). This layer introduces
 * the IR type + `evaluate_at(t)` method with all four interpolation
 * modes. Loader integration (parse `keyframes` form) and Transform
 * struct migration (8 fields from `double` to `AnimatedNumber`)
 * are separately-scoped follow-up cycles.
 *
 * Contract:
 *   - `static_value` set XOR `keyframes` non-empty. Both populated
 *     is caller error (not asserted; evaluate_at prefers static
 *     if present for defensiveness).
 *   - Keyframes sorted by t, no duplicates — caller (loader)
 *     enforces. `evaluate_at` trusts the invariant.
 *   - Before first kf: returns first kf's v (no extrapolation
 *     per TIMELINE_SCHEMA.md).
 *   - After last kf: returns last kf's v.
 *   - Within [a.t, b.t): interpolates by `a.interp`:
 *     - Linear: a.v + u * (b.v - a.v), u = (t - a.t) / (b.t - a.t).
 *     - Hold: a.v (value freezes at segment start).
 *     - Stepped: a.v (same as hold for numerical properties;
 *       schema distinction is UI intent, math collapses).
 *     - Bezier: solve CSS cubic-bezier with control points
 *       `a.cp = [x1, y1, x2, y2]` via Newton iteration on x.
 */
#pragma once

#include "media_engine/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace me {

enum class Interp : std::uint8_t {
    Linear  = 0,
    Bezier  = 1,
    Hold    = 2,
    Stepped = 3,
};

struct Keyframe {
    me_rational_t        t{0, 1};
    double               v = 0.0;
    Interp               interp = Interp::Linear;
    /* CSS cubic-bezier control points [x1, y1, x2, y2] — relevant
     * iff interp == Bezier. x1 / x2 clamped to [0,1] by evaluator
     * to guarantee monotonic mapping from time to parameter. */
    std::array<double,4> cp{0.0, 0.0, 1.0, 1.0};
};

struct AnimatedNumber {
    std::optional<double>    static_value;
    std::vector<Keyframe>    keyframes;

    static AnimatedNumber from_static(double v) {
        AnimatedNumber a;
        a.static_value = v;
        return a;
    }
    static AnimatedNumber from_keyframes(std::vector<Keyframe> kfs) {
        AnimatedNumber a;
        a.keyframes = std::move(kfs);
        return a;
    }

    /* Returns the value at composition-time `t`. `t` uses
     * Timeline's rational time convention (num/den). For static
     * AnimatedNumbers, `t` is ignored. For keyframed ones, the
     * kf list is assumed sorted by .t with no duplicates; empty
     * list returns 0.0 (caller should validate). */
    double evaluate_at(me_rational_t t) const;
};

}  // namespace me
