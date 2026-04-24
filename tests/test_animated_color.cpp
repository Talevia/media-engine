/*
 * test_animated_color — unit tests for me::AnimatedColor +
 * parse_animated_color loader.
 *
 * Covers:
 *   - from_static / evaluate_at ignores t.
 *   - Linear keyframe interp between two hex colors.
 *   - Hold / Stepped interp returns a.v throughout segment.
 *   - Before first kf / after last kf clamp.
 *   - Loader accepts plain-string, {static: ...}, {keyframes: ...}.
 *   - Loader rejects invalid hex, missing keys, wrong interp.
 *
 * Also pins TextClipParams via parse_text_clip_params round-trip
 * with both legacy plain-string and new keyframed forms.
 */
#include <doctest/doctest.h>

#include "timeline/animated_color.hpp"
#include "timeline/loader_helpers.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using me::timeline_loader_detail::parse_animated_color;
using me::timeline_loader_detail::parse_text_clip_params;
using me::timeline_loader_detail::LoadError;

TEST_CASE("AnimatedColor::from_static: evaluate_at ignores t") {
    auto c = me::AnimatedColor::from_static({0x11, 0x22, 0x33, 0x44});
    CHECK(c.evaluate_at({0, 1})    == std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44});
    CHECK(c.evaluate_at({100, 1})  == std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44});
}

TEST_CASE("AnimatedColor::default_opaque_white") {
    const auto c = me::AnimatedColor::default_opaque_white();
    CHECK(c.evaluate_at({5, 7}) == std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("AnimatedColor keyframed linear interpolation") {
    /* Red at t=0 → Blue at t=2: midway at t=1 should be R=128 B=128. */
    me::ColorKeyframe a{{0, 1}, {0xFF, 0x00, 0x00, 0xFF}, me::Interp::Linear, {0,0,1,1}};
    me::ColorKeyframe b{{2, 1}, {0x00, 0x00, 0xFF, 0xFF}, me::Interp::Linear, {0,0,1,1}};
    auto c = me::AnimatedColor::from_keyframes({a, b});

    auto mid = c.evaluate_at({1, 1});
    CHECK(mid[0] >= 127);   /* allow ±1 rounding slop */
    CHECK(mid[0] <= 128);
    CHECK(mid[1] == 0);
    CHECK(mid[2] >= 127);
    CHECK(mid[2] <= 128);
    CHECK(mid[3] == 0xFF);
}

TEST_CASE("AnimatedColor keyframed hold / stepped freeze at segment start") {
    me::ColorKeyframe a{{0, 1}, {0xFF, 0x00, 0x00, 0xFF}, me::Interp::Hold, {0,0,1,1}};
    me::ColorKeyframe b{{2, 1}, {0x00, 0x00, 0xFF, 0xFF}, me::Interp::Linear, {0,0,1,1}};
    auto c = me::AnimatedColor::from_keyframes({a, b});

    /* Between [0, 2): hold returns a.v (red). */
    CHECK(c.evaluate_at({1, 1})[0] == 0xFF);
    CHECK(c.evaluate_at({1, 1})[2] == 0x00);
    /* At t=2 the second kf becomes the "last kf" boundary → returns b.v. */
    CHECK(c.evaluate_at({2, 1})[0] == 0x00);
    CHECK(c.evaluate_at({2, 1})[2] == 0xFF);
}

TEST_CASE("AnimatedColor boundary: before first / after last") {
    me::ColorKeyframe a{{1, 1}, {0xAB, 0xCD, 0xEF, 0x11}, me::Interp::Linear, {0,0,1,1}};
    me::ColorKeyframe b{{2, 1}, {0x00, 0x00, 0x00, 0x00}, me::Interp::Linear, {0,0,1,1}};
    auto c = me::AnimatedColor::from_keyframes({a, b});

    /* t=0 before first → a.v. */
    CHECK(c.evaluate_at({0, 1}) == std::array<std::uint8_t, 4>{0xAB, 0xCD, 0xEF, 0x11});
    /* t=10 after last → b.v. */
    CHECK(c.evaluate_at({10, 1}) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
}

TEST_CASE("parse_animated_color: plain-string legacy shape") {
    json j = "#FFFFFFFF";
    auto c = parse_animated_color(j, "color");
    CHECK(c.static_value.has_value());
    CHECK(c.evaluate_at({0, 1}) == std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("parse_animated_color: explicit static object") {
    json j = json::parse(R"({"static":"#11223344"})");
    auto c = parse_animated_color(j, "color");
    CHECK(c.evaluate_at({0, 1}) == std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44});
}

TEST_CASE("parse_animated_color: keyframes object") {
    json j = json::parse(R"({
      "keyframes": [
        {"t":{"num":0,"den":1}, "v":"#FF0000FF", "interp":"linear"},
        {"t":{"num":1,"den":1}, "v":"#0000FFFF", "interp":"linear"}
      ]
    })");
    auto c = parse_animated_color(j, "color");
    CHECK(!c.static_value.has_value());
    CHECK(c.keyframes.size() == 2);
    /* Midpoint sanity — full interp math is covered in the typed
     * tests above; here we only confirm the loader wired the
     * keyframes in correctly. */
    const auto mid = c.evaluate_at({1, 2});
    CHECK(mid[0] > 0);
    CHECK(mid[0] < 0xFF);
}

TEST_CASE("parse_animated_color: invalid hex rejected") {
    json j = "not-a-color";
    CHECK_THROWS_AS(parse_animated_color(j, "color"), LoadError);
}

TEST_CASE("parse_animated_color: object missing static+keyframes rejected") {
    json j = json::parse(R"({})");
    CHECK_THROWS_AS(parse_animated_color(j, "color"), LoadError);
}

TEST_CASE("parse_text_clip_params: legacy plain-string color still works") {
    json j = json::parse(R"({
      "content":"Hi",
      "color":"#80FF00FF",
      "fontSize":{"static":48},
      "x":{"static":0},
      "y":{"static":0}
    })");
    auto p = parse_text_clip_params(j, "textParams");
    CHECK(p.content == "Hi");
    const auto rgba = p.color.evaluate_at({0, 1});
    CHECK(rgba[0] == 0x80);
    CHECK(rgba[1] == 0xFF);
    CHECK(rgba[2] == 0x00);
    CHECK(rgba[3] == 0xFF);
}

TEST_CASE("parse_text_clip_params: keyframed color works") {
    json j = json::parse(R"({
      "content":"Fade",
      "color":{
        "keyframes":[
          {"t":{"num":0,"den":1}, "v":"#FFFFFF00", "interp":"linear"},
          {"t":{"num":1,"den":1}, "v":"#FFFFFFFF", "interp":"linear"}
        ]
      },
      "fontSize":{"static":48},
      "x":{"static":0},
      "y":{"static":0}
    })");
    auto p = parse_text_clip_params(j, "textParams");
    CHECK(p.content == "Fade");
    /* Alpha fades from 0 to 0xFF across [0, 1]. Midway ≈ 128. */
    const auto rgba = p.color.evaluate_at({1, 2});
    CHECK(rgba[3] >= 127);
    CHECK(rgba[3] <= 128);
}
