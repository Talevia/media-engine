/*
 * test_animated_number_loader — contract pins for the JSON →
 * `me::AnimatedNumber` parse path (layer 2 of
 * `transform-animated-support`). Exercises
 * `me::timeline_loader_detail::parse_animated_number` directly —
 * not yet wired into parse_transform / Clip::gain_db, so a
 * top-level me_timeline_load_json test wouldn't reach this code.
 */
#include <doctest/doctest.h>

#include "timeline/loader_helpers.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using me::timeline_loader_detail::LoadError;
using me::timeline_loader_detail::parse_animated_number;
using json = nlohmann::json;

namespace {

me::AnimatedNumber parse_or_fail(const std::string& src, const char* where = "p") {
    return parse_animated_number(json::parse(src), where);
}

bool throws_with_status(me_status_t expected, const std::string& src,
                        const char* where = "p") {
    try {
        (void)parse_or_fail(src, where);
        return false;
    } catch (const LoadError& e) {
        return e.status == expected;
    } catch (...) {
        return false;
    }
}

}  // namespace

TEST_CASE("parse_animated_number: static form returns AnimatedNumber with static_value set") {
    auto a = parse_or_fail(R"({"static": 0.5})");
    REQUIRE(a.static_value.has_value());
    CHECK(*a.static_value == doctest::Approx(0.5));
    CHECK(a.keyframes.empty());
    CHECK(a.evaluate_at(me_rational_t{100, 30}) == doctest::Approx(0.5));
}

TEST_CASE("parse_animated_number: linear keyframes form") {
    auto a = parse_or_fail(R"({
        "keyframes": [
            {"t": {"num": 0,  "den": 30}, "v": 0.0, "interp": "linear"},
            {"t": {"num": 30, "den": 30}, "v": 1.0, "interp": "linear"}
        ]
    })");
    CHECK_FALSE(a.static_value.has_value());
    REQUIRE(a.keyframes.size() == 2);
    CHECK(a.keyframes[0].v == doctest::Approx(0.0));
    CHECK(a.keyframes[1].v == doctest::Approx(1.0));
    CHECK(a.keyframes[0].interp == me::Interp::Linear);
    /* Midpoint == 0.5 (AnimatedNumber::evaluate_at pinned separately). */
    CHECK(a.evaluate_at(me_rational_t{15, 30}) == doctest::Approx(0.5));
}

TEST_CASE("parse_animated_number: bezier with cp produces correct keyframe") {
    auto a = parse_or_fail(R"({
        "keyframes": [
            {"t": {"num": 0,  "den": 30}, "v": 0.0,
             "interp": "bezier", "cp": [0.42, 0, 0.58, 1]},
            {"t": {"num": 100, "den": 30}, "v": 1.0, "interp": "linear"}
        ]
    })");
    REQUIRE(a.keyframes.size() == 2);
    CHECK(a.keyframes[0].interp == me::Interp::Bezier);
    CHECK(a.keyframes[0].cp[0] == doctest::Approx(0.42));
    CHECK(a.keyframes[0].cp[1] == doctest::Approx(0.0));
    CHECK(a.keyframes[0].cp[2] == doctest::Approx(0.58));
    CHECK(a.keyframes[0].cp[3] == doctest::Approx(1.0));
}

TEST_CASE("parse_animated_number: hold / stepped interp parses") {
    auto ah = parse_or_fail(R"({
        "keyframes": [
            {"t": {"num": 0,  "den": 30}, "v": 10.0, "interp": "hold"},
            {"t": {"num": 30, "den": 30}, "v": 20.0, "interp": "linear"}
        ]
    })");
    CHECK(ah.keyframes[0].interp == me::Interp::Hold);

    auto as = parse_or_fail(R"({
        "keyframes": [
            {"t": {"num": 0,  "den": 30}, "v": 100.0, "interp": "stepped"},
            {"t": {"num": 30, "den": 30}, "v": 200.0, "interp": "linear"}
        ]
    })");
    CHECK(as.keyframes[0].interp == me::Interp::Stepped);
}

TEST_CASE("parse_animated_number: rejects neither static nor keyframes (empty object)") {
    CHECK(throws_with_status(ME_E_PARSE, R"({})"));
}

TEST_CASE("parse_animated_number: rejects both static and keyframes present") {
    CHECK(throws_with_status(ME_E_PARSE, R"({
        "static": 0.5,
        "keyframes": [{"t": {"num": 0, "den": 30}, "v": 0.0, "interp": "linear"}]
    })"));
}

TEST_CASE("parse_animated_number: rejects unknown interp") {
    CHECK(throws_with_status(ME_E_PARSE, R"({
        "keyframes": [{"t": {"num": 0, "den": 30}, "v": 0.0, "interp": "smoothstep"}]
    })"));
}

TEST_CASE("parse_animated_number: rejects bezier without cp") {
    CHECK(throws_with_status(ME_E_PARSE, R"({
        "keyframes": [
            {"t": {"num": 0, "den": 30}, "v": 0.0, "interp": "bezier"},
            {"t": {"num": 30, "den": 30}, "v": 1.0, "interp": "linear"}
        ]
    })"));
}

TEST_CASE("parse_animated_number: rejects bezier cp x1 out of [0,1]") {
    CHECK(throws_with_status(ME_E_PARSE, R"({
        "keyframes": [
            {"t": {"num": 0, "den": 30}, "v": 0.0,
             "interp": "bezier", "cp": [1.5, 0, 0.58, 1]},
            {"t": {"num": 30, "den": 30}, "v": 1.0, "interp": "linear"}
        ]
    })"));
}

TEST_CASE("parse_animated_number: rejects unsorted keyframes") {
    CHECK(throws_with_status(ME_E_PARSE, R"({
        "keyframes": [
            {"t": {"num": 30, "den": 30}, "v": 1.0, "interp": "linear"},
            {"t": {"num": 0,  "den": 30}, "v": 0.0, "interp": "linear"}
        ]
    })"));
}

TEST_CASE("parse_animated_number: rejects duplicate t") {
    CHECK(throws_with_status(ME_E_PARSE, R"({
        "keyframes": [
            {"t": {"num": 10, "den": 30}, "v": 0.0, "interp": "linear"},
            {"t": {"num": 10, "den": 30}, "v": 1.0, "interp": "linear"}
        ]
    })"));
}

TEST_CASE("parse_animated_number: rejects empty keyframes array") {
    CHECK(throws_with_status(ME_E_PARSE, R"({ "keyframes": [] })"));
}

TEST_CASE("parse_animated_number: rejects unknown keyframe key") {
    CHECK(throws_with_status(ME_E_PARSE, R"({
        "keyframes": [{"t":{"num":0,"den":30}, "v":0.0, "interp":"linear", "foo":42}]
    })"));
}
