/*
 * test_timeline_schema_transitions — extracted from
 * test_timeline_schema.cpp when that file was split by schema
 * section (debt-split-test-timeline-schema-cpp). Shared fixtures
 * live in timeline_schema_fixtures.hpp.
 */
#include "timeline_schema_fixtures.hpp"

#include <string>
#include <string_view>

using me::tests::schema::load;
using me::tests::schema::EngineFixture;

TEST_CASE("track.transitions with crossDissolve between adjacent clips loads into IR") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ],"transitions":[
          {"kind":"crossDissolve","fromClipId":"c1","toClipId":"c2",
           "duration":{"num":15,"den":30}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->tl.transitions.size() == 1);
    const auto& t = tl->tl.transitions[0];
    CHECK(t.kind == me::TransitionKind::CrossDissolve);
    CHECK(t.track_id == "v0");
    CHECK(t.from_clip_id == "c1");
    CHECK(t.to_clip_id == "c2");
    CHECK(t.duration.num == 15);
    CHECK(t.duration.den == 30);
    me_timeline_destroy(tl);
}

TEST_CASE("non-empty transitions route through compose sink; passthrough codec rejected") {
    /* Previously, Exporter flat-rejected any timeline with transitions
     * (before cross-dissolve kernel existed). Now transitions route
     * through ComposeSink, which requires h264/aac encoding. The
     * test keeps its passthrough spec to verify the precise surface
     * where rejection happens: the compose factory's codec gate. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ],"transitions":[
          {"kind":"crossDissolve","fromClipId":"c1","toClipId":"c2",
           "duration":{"num":15,"den":30}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    me_output_spec_t spec{};
    spec.path = "/tmp/me-transition-reject.mp4";
    spec.container = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";
    me_render_job_t* job = nullptr;
    CHECK(me_render_start(f.eng, tl, &spec, nullptr, nullptr, &job) == ME_E_UNSUPPORTED);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("compose path") != std::string::npos);
    CHECK(std::string{err}.find("h264") != std::string::npos);
    me_timeline_destroy(tl);
}

TEST_CASE("transition with unknown kind rejected as ME_E_UNSUPPORTED") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ],"transitions":[
          {"kind":"wipe","fromClipId":"c1","toClipId":"c2",
           "duration":{"num":15,"den":30}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_UNSUPPORTED);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("only 'crossDissolve'") != std::string::npos);
}

TEST_CASE("transition with unknown fromClipId rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ],"transitions":[
          {"kind":"crossDissolve","fromClipId":"c-bogus","toClipId":"c2",
           "duration":{"num":15,"den":30}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("fromClipId refers to unknown clip") != std::string::npos);
}

TEST_CASE("transition between non-adjacent clips rejected as ME_E_PARSE") {
    EngineFixture f;
    /* 3 clips: c1→c2→c3. Valid transition is c1→c2 or c2→c3; c1→c3 skips c2. */
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c3","assetId":"a1",
           "timeRange":{"start":{"num":120,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ],"transitions":[
          {"kind":"crossDissolve","fromClipId":"c1","toClipId":"c3",
           "duration":{"num":15,"den":30}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("immediately follow fromClipId") != std::string::npos);
}

TEST_CASE("transition duration longer than adjacent clips rejected as ME_E_PARSE") {
    EngineFixture f;
    /* Both clips are 60/30 = 2s; transition duration 120/30 = 4s > 2s. */
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ],"transitions":[
          {"kind":"crossDissolve","fromClipId":"c1","toClipId":"c2",
           "duration":{"num":120,"den":30}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("must not exceed either adjacent clip") != std::string::npos);
}

TEST_CASE("transition with non-positive duration rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ],"transitions":[
          {"kind":"crossDissolve","fromClipId":"c1","toClipId":"c2",
           "duration":{"num":0,"den":30}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("duration must be positive") != std::string::npos);
}

TEST_CASE("absent transitions array leaves Timeline::transitions empty") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string j = tb::minimal_video_clip().build();
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    CHECK(tl->tl.transitions.empty());
    me_timeline_destroy(tl);
}

