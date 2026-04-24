/*
 * test_animated_number — interpolation contract pins for
 * me::AnimatedNumber (the M3 transform-animated-support primitive).
 *
 * Covers: static passthrough, before-first-kf clamp, after-last-kf
 * clamp, single-kf degenerate, linear mid / bounds, hold, stepped,
 * bezier endpoints + midpoint, multi-segment chain with mixed
 * interp modes.
 */
#include <doctest/doctest.h>

#include "timeline/animated_number.hpp"

#include <vector>

using me::AnimatedNumber;
using me::Interp;
using me::Keyframe;

namespace {

Keyframe kf(int64_t t_num, int64_t t_den, double v, Interp i = Interp::Linear,
            double x1 = 0.0, double y1 = 0.0, double x2 = 1.0, double y2 = 1.0) {
    Keyframe k;
    k.t = me_rational_t{t_num, t_den};
    k.v = v;
    k.interp = i;
    k.cp = {x1, y1, x2, y2};
    return k;
}

}  // namespace

TEST_CASE("AnimatedNumber: static value ignores time") {
    auto a = AnimatedNumber::from_static(0.42);
    CHECK(a.evaluate_at(me_rational_t{0, 1})    == doctest::Approx(0.42));
    CHECK(a.evaluate_at(me_rational_t{100, 30}) == doctest::Approx(0.42));
    CHECK(a.evaluate_at(me_rational_t{-5, 1})   == doctest::Approx(0.42));
}

TEST_CASE("AnimatedNumber: empty keyframes list returns 0.0 (defensive)") {
    AnimatedNumber a;
    a.keyframes = {};  /* neither static nor keyframes */
    CHECK(a.evaluate_at(me_rational_t{10, 30}) == doctest::Approx(0.0));
}

TEST_CASE("AnimatedNumber: single keyframe returns its value at any time") {
    auto a = AnimatedNumber::from_keyframes({
        kf(30, 30, 7.5)
    });
    CHECK(a.evaluate_at(me_rational_t{0, 30})   == doctest::Approx(7.5));
    CHECK(a.evaluate_at(me_rational_t{30, 30})  == doctest::Approx(7.5));
    CHECK(a.evaluate_at(me_rational_t{100, 30}) == doctest::Approx(7.5));
}

TEST_CASE("AnimatedNumber: before first keyframe returns first.v") {
    auto a = AnimatedNumber::from_keyframes({
        kf(30, 30, 1.0),
        kf(60, 30, 2.0),
    });
    CHECK(a.evaluate_at(me_rational_t{0, 30})  == doctest::Approx(1.0));
    CHECK(a.evaluate_at(me_rational_t{15, 30}) == doctest::Approx(1.0));
}

TEST_CASE("AnimatedNumber: after last keyframe returns last.v") {
    auto a = AnimatedNumber::from_keyframes({
        kf(0, 30, 1.0),
        kf(30, 30, 2.0),
    });
    CHECK(a.evaluate_at(me_rational_t{60, 30}) == doctest::Approx(2.0));
    CHECK(a.evaluate_at(me_rational_t{30, 30}) == doctest::Approx(2.0));  /* == last.t */
}

TEST_CASE("AnimatedNumber: linear interp midpoint") {
    auto a = AnimatedNumber::from_keyframes({
        kf(0, 30,  0.0, Interp::Linear),
        kf(30, 30, 1.0, Interp::Linear),
    });
    CHECK(a.evaluate_at(me_rational_t{15, 30}) == doctest::Approx(0.5));
    CHECK(a.evaluate_at(me_rational_t{10, 30}) == doctest::Approx(1.0 / 3.0).epsilon(1e-9));
    CHECK(a.evaluate_at(me_rational_t{0, 30})  == doctest::Approx(0.0));
}

TEST_CASE("AnimatedNumber: hold keeps value at segment start") {
    auto a = AnimatedNumber::from_keyframes({
        kf(0, 30,  10.0, Interp::Hold),
        kf(30, 30, 20.0, Interp::Linear),
    });
    CHECK(a.evaluate_at(me_rational_t{0, 30})  == doctest::Approx(10.0));
    CHECK(a.evaluate_at(me_rational_t{5, 30})  == doctest::Approx(10.0));
    CHECK(a.evaluate_at(me_rational_t{29, 30}) == doctest::Approx(10.0));
    /* At or past next kf → second segment's interp would be Linear
     * starting at (30, 20). Since kf[1] is the last, >= 30 → 20. */
    CHECK(a.evaluate_at(me_rational_t{30, 30}) == doctest::Approx(20.0));
}

TEST_CASE("AnimatedNumber: stepped behaves as hold for numeric values") {
    auto a = AnimatedNumber::from_keyframes({
        kf(0, 30,  100.0, Interp::Stepped),
        kf(30, 30, 200.0, Interp::Linear),
    });
    CHECK(a.evaluate_at(me_rational_t{0, 30})  == doctest::Approx(100.0));
    CHECK(a.evaluate_at(me_rational_t{15, 30}) == doctest::Approx(100.0));
    CHECK(a.evaluate_at(me_rational_t{30, 30}) == doctest::Approx(200.0));
}

TEST_CASE("AnimatedNumber: bezier endpoints exact, midpoint matches linear when cp is default") {
    /* Default cp = (0,0) to (1,1) → identical to linear y=x
     * parameterization. Midpoint should match linear 0.5 exactly. */
    auto a = AnimatedNumber::from_keyframes({
        kf(0, 30,  0.0, Interp::Bezier, 0.0, 0.0, 1.0, 1.0),
        kf(30, 30, 1.0, Interp::Linear),
    });
    CHECK(a.evaluate_at(me_rational_t{0, 30})  == doctest::Approx(0.0));
    CHECK(a.evaluate_at(me_rational_t{30, 30}) == doctest::Approx(1.0));
    /* With cp = linear, midpoint should be ~0.5 (bezier y at x=0.5
     * with (0,0) (0,0) (1,1) (1,1) cp is 0.5). */
    CHECK(a.evaluate_at(me_rational_t{15, 30}) == doctest::Approx(0.5).epsilon(1e-3));
}

TEST_CASE("AnimatedNumber: bezier ease-in-out cp curves the output") {
    /* CSS ease-in-out cubic-bezier: (0.42, 0, 0.58, 1).
     * At x=0.5 (midpoint), y should be ~0.5 by symmetry;
     * at x=0.25, y < 0.25 (ease-in: slow start);
     * at x=0.75, y > 0.75 (ease-out accelerated by then). */
    auto a = AnimatedNumber::from_keyframes({
        kf(0, 30,   0.0, Interp::Bezier, 0.42, 0.0, 0.58, 1.0),
        kf(100, 30, 1.0, Interp::Linear),
    });
    /* Midpoint symmetry. */
    CHECK(a.evaluate_at(me_rational_t{50, 30})
          == doctest::Approx(0.5).epsilon(1e-2));
    /* Ease-in: at u=0.25, y should be noticeably < 0.25. */
    const double y25 = a.evaluate_at(me_rational_t{25, 30});
    CHECK(y25 < 0.25);
    CHECK(y25 > 0.0);
    /* Ease-out: at u=0.75, y should be > 0.75. */
    const double y75 = a.evaluate_at(me_rational_t{75, 30});
    CHECK(y75 > 0.75);
    CHECK(y75 < 1.0);
}

TEST_CASE("AnimatedNumber: multi-segment chain with mixed interp modes") {
    /* kf0 -> kf1 linear; kf1 -> kf2 hold; kf2 -> kf3 linear. */
    auto a = AnimatedNumber::from_keyframes({
        kf(0,  30, 0.0,  Interp::Linear),
        kf(30, 30, 10.0, Interp::Hold),
        kf(60, 30, 10.0, Interp::Linear),
        kf(90, 30, 0.0,  Interp::Linear),
    });
    /* Segment 0 → midpoint = 5.0. */
    CHECK(a.evaluate_at(me_rational_t{15, 30}) == doctest::Approx(5.0));
    /* Segment 1 (hold) → 10 anywhere in [30,60). */
    CHECK(a.evaluate_at(me_rational_t{30, 30}) == doctest::Approx(10.0));
    CHECK(a.evaluate_at(me_rational_t{45, 30}) == doctest::Approx(10.0));
    /* Segment 2 → midpoint = 5.0 again (10 down to 0). */
    CHECK(a.evaluate_at(me_rational_t{75, 30}) == doctest::Approx(5.0));
    /* Past last → 0. */
    CHECK(a.evaluate_at(me_rational_t{100, 30}) == doctest::Approx(0.0));
}
