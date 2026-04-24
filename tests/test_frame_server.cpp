/*
 * test_frame_server — pixel-proof tests for the M6 frame-server
 * core path (me_render_frame → Previewer::frame_at → RGBA8
 * me_frame).
 *
 * Uses the determinism_fixture MP4 (2s / 30fps deterministic
 * video) wrapped in a single-clip timeline. Asserts dimensions
 * + non-transparent pixels + accessor round-trip.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef ME_TEST_FIXTURE_MP4
#error "ME_TEST_FIXTURE_MP4 must be defined via CMake"
#endif

namespace {

std::string build_single_clip_json(const char* uri) {
    /* Timeline: single 2s video clip of the fixture. frame_rate
     * matches the fixture's 30fps. */
    std::string s = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":160,"height":120},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a0","uri":")";
    s += uri;
    s += R"("}],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"video","clips":[
            {"id":"c0","type":"video","assetId":"a0",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}},
             "sourceRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}}}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    return s;
}

struct TimelineRAII {
    me_engine_t*   eng = nullptr;
    me_timeline_t* tl  = nullptr;
    TimelineRAII() {
        me_engine_create(nullptr, &eng);
        const std::string uri = "file://" + std::string(ME_TEST_FIXTURE_MP4);
        const std::string js  = build_single_clip_json(uri.c_str());
        me_timeline_load_json(eng, js.data(), js.size(), &tl);
    }
    ~TimelineRAII() {
        if (tl)  me_timeline_destroy(tl);
        if (eng) me_engine_destroy(eng);
    }
};

}  // namespace

TEST_CASE("me_render_frame: returns valid RGBA8 frame from fixture") {
    TimelineRAII f;
    REQUIRE(f.eng);
    REQUIRE(f.tl);

    me_frame_t* frame = nullptr;
    const me_status_t s = me_render_frame(
        f.eng, f.tl, me_rational_t{1, 1},  /* t = 1.0s */
        &frame);

    REQUIRE(s == ME_OK);
    REQUIRE(frame != nullptr);

    const int w = me_frame_width(frame);
    const int h = me_frame_height(frame);
    CHECK(w > 0);
    CHECK(h > 0);

    const uint8_t* px = me_frame_pixels(frame);
    REQUIRE(px != nullptr);

    /* At least one pixel has non-zero alpha — frame actually
     * contains decoded content. */
    bool any_alpha = false;
    for (int i = 3; i < w * h * 4; i += 4) {
        if (px[i] != 0) { any_alpha = true; break; }
    }
    CHECK(any_alpha);

    me_frame_destroy(frame);
}

TEST_CASE("me_render_frame: NULL args return ME_E_INVALID_ARG") {
    TimelineRAII f;
    me_frame_t* frame = nullptr;

    CHECK(me_render_frame(nullptr, f.tl, me_rational_t{0, 1}, &frame) == ME_E_INVALID_ARG);
    CHECK(me_render_frame(f.eng, nullptr, me_rational_t{0, 1}, &frame) == ME_E_INVALID_ARG);
    CHECK(me_render_frame(f.eng, f.tl, me_rational_t{0, 1}, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("me_render_frame: time past timeline duration → ME_E_NOT_FOUND") {
    TimelineRAII f;
    me_frame_t* frame = nullptr;
    /* Timeline is 2s; t=5s is past end. */
    const me_status_t s = me_render_frame(
        f.eng, f.tl, me_rational_t{5, 1}, &frame);
    CHECK(s == ME_E_NOT_FOUND);
    CHECK(frame == nullptr);
}

TEST_CASE("me_frame accessors: NULL-safe + destroy is idempotent-safe") {
    /* me_frame_* all accept NULL gracefully — common pattern for
     * opaque-handle C APIs. */
    CHECK(me_frame_width(nullptr)  == 0);
    CHECK(me_frame_height(nullptr) == 0);
    CHECK(me_frame_pixels(nullptr) == nullptr);
    me_frame_destroy(nullptr);  /* must not crash */
}

TEST_CASE("me_render_frame: dimensions match fixture W×H") {
    TimelineRAII f;
    me_frame_t* frame = nullptr;
    REQUIRE(me_render_frame(f.eng, f.tl, me_rational_t{0, 1}, &frame) == ME_OK);
    REQUIRE(frame != nullptr);

    /* Fixture is generated at 640×480 by gen_fixture
     * (verified on dev machine; mirror the determinism_fixture
     * target's settings if the fixture is rebuilt at a
     * different resolution). */
    CHECK(me_frame_width(frame) == 640);
    CHECK(me_frame_height(frame) == 480);

    me_frame_destroy(frame);
}
