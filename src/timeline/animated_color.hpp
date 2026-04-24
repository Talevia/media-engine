/*
 * me::AnimatedColor — per-channel RGBA value-over-time primitive.
 *
 * Companion to `me::AnimatedNumber`; same JSON shape
 * (`{"static": <hex>}` vs `{"keyframes": [...]}`), same `Interp`
 * enum, but values are 4-byte RGBA (uint8_t[4]) rather than
 * double. Interpolation runs independently per channel in linear
 * uint8 space — `lerp(a.r, b.r, u)`, etc.
 *
 * Use case: TextClipParams::color was static `std::string` (hex)
 * through cycle 39; `text-clip-color-animation` (cycle 40)
 * migrates it to AnimatedColor so text fades / cross-tint
 * animations work without a second animated-property primitive.
 *
 * JSON at the schema level:
 *
 *   "color": "#FFFFFFFF"                      (legacy plain-string form)
 *   "color": { "static": "#FFFFFFFF" }        (explicit static, new)
 *   "color": { "keyframes": [
 *       { "t": {"num":0,"den":1}, "v": "#FF0000FF", "interp": "linear" },
 *       { "t": {"num":2,"den":1}, "v": "#0000FFFF", "interp": "linear" }
 *   ]}                                          (animated form, new)
 *
 * The loader (parse_animated_color) accepts all three shapes.
 * Plain-string is sugar for `{"static": ...}` and preserved
 * indefinitely — existing timelines keep working.
 *
 * Interpolation math: identical bracketing logic as AnimatedNumber
 * (see animated_number.cpp::evaluate_at). For Bezier keyframes,
 * the warped fraction is computed once and applied to each of
 * R/G/B/A independently. Values rounded to the nearest uint8.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/animated_number.hpp"   /* for Interp enum */

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace me {

struct ColorKeyframe {
    me_rational_t          t{0, 1};
    /* RGBA little-endian byte order: [0]=R, [1]=G, [2]=B, [3]=A. */
    std::array<std::uint8_t, 4> v{0xFF, 0xFF, 0xFF, 0xFF};
    Interp                 interp = Interp::Linear;
    std::array<double, 4>  cp{0.0, 0.0, 1.0, 1.0};
};

struct AnimatedColor {
    std::optional<std::array<std::uint8_t, 4>> static_value;
    std::vector<ColorKeyframe>                  keyframes;

    static AnimatedColor from_static(std::array<std::uint8_t, 4> rgba) {
        AnimatedColor c;
        c.static_value = rgba;
        return c;
    }
    static AnimatedColor from_keyframes(std::vector<ColorKeyframe> kfs) {
        AnimatedColor c;
        c.keyframes = std::move(kfs);
        return c;
    }

    /* Default = opaque white, matches TextClipParams's previous
     * string default of "#FFFFFFFF". */
    static AnimatedColor default_opaque_white() {
        return from_static({0xFF, 0xFF, 0xFF, 0xFF});
    }

    /* Evaluate at composition-time t. Contract mirrors
     * AnimatedNumber::evaluate_at — static ignores t; keyframed
     * returns first kf before the range / last kf after; between
     * adjacent kfs interpolates per-channel by the earlier kf's
     * `interp` mode. Empty kf list (shouldn't happen under
     * loader invariants) returns opaque white. */
    std::array<std::uint8_t, 4> evaluate_at(me_rational_t t) const;
};

}  // namespace me
