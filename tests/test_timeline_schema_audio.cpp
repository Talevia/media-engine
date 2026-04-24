/*
 * test_timeline_schema_audio — extracted from
 * test_timeline_schema.cpp when that file was split by schema
 * section (debt-split-test-timeline-schema-cpp). Shared fixtures
 * live in timeline_schema_fixtures.hpp.
 */
#include "timeline_schema_fixtures.hpp"

#include <string>
#include <string_view>

using me::tests::schema::load;
using me::tests::schema::EngineFixture;

TEST_CASE("audio track with audio clip + static gainDb loads into IR") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"audio","uri":"file:///tmp/audio.wav"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "sourceRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "gainDb":{"static":-6.0}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->tl.tracks.size() == 1);
    CHECK(tl->tl.tracks[0].kind == me::TrackKind::Audio);
    REQUIRE(tl->tl.clips.size() == 1);
    CHECK(tl->tl.clips[0].type == me::ClipType::Audio);
    REQUIRE(tl->tl.clips[0].gain_db.has_value());
    CHECK(tl->tl.clips[0].gain_db->static_value.has_value());
    CHECK(*tl->tl.clips[0].gain_db->static_value == -6.0);
    CHECK_FALSE(tl->tl.clips[0].transform.has_value());
    me_timeline_destroy(tl);
}

TEST_CASE("audio track with audio clip + keyframed gainDb loads into IR") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"audio","uri":"file:///tmp/audio.wav"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "sourceRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "gainDb":{"keyframes":[
             {"t":{"num":0,"den":48000},    "v":0.0,   "interp":"linear"},
             {"t":{"num":48000,"den":48000},"v":-20.0, "interp":"linear"}
           ]}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->tl.clips[0].gain_db.has_value());
    /* Keyframed form: static_value empty, keyframes populated. */
    CHECK_FALSE(tl->tl.clips[0].gain_db->static_value.has_value());
    REQUIRE(tl->tl.clips[0].gain_db->keyframes.size() == 2);
    CHECK(tl->tl.clips[0].gain_db->keyframes[0].v == 0.0);
    CHECK(tl->tl.clips[0].gain_db->keyframes[1].v == -20.0);
    /* Midpoint evaluation pins that evaluate_at plugs into the same
     * kernel as Transform's AnimatedNumber fields. */
    CHECK(tl->tl.clips[0].gain_db->evaluate_at(me_rational_t{24000, 48000})
          == doctest::Approx(-10.0));
    me_timeline_destroy(tl);
}

TEST_CASE("audio track without gainDb loads with gain_db == nullopt") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"audio","uri":"file:///tmp/audio.wav"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "sourceRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    CHECK(tl->tl.clips[0].type == me::ClipType::Audio);
    CHECK_FALSE(tl->tl.clips[0].gain_db.has_value());
    me_timeline_destroy(tl);
}

TEST_CASE("standalone audio track routes through compose; passthrough codec still rejected") {
    /* Pre-audio-mix-scheduler-wire this was rejected at Exporter
     * with "standalone audio tracks not yet implemented". Now the
     * compose path handles audio tracks via AudioMixer, so the
     * Exporter gate is gone. Passthrough codec still unsupported
     * on the compose path — rejection surfaces at the compose
     * factory level instead. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"audio","uri":"file:///tmp/audio.wav"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "sourceRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);

    me_output_spec_t spec{};
    spec.path = "/tmp/me-audio-reject.mp4";
    spec.container = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";
    me_render_job_t* job = nullptr;
    CHECK(me_render_start(f.eng, tl, &spec, nullptr, nullptr, &job) == ME_E_UNSUPPORTED);
    CHECK(job == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    /* Audio-only timeline routes through AudioOnlySink (landed in
     * audio-only-timeline-support cycle). Passthrough audio codec
     * rejected there; h264 video codec unused in audio-only. */
    CHECK(std::string{err}.find("audio-only path") != std::string::npos);
    CHECK(std::string{err}.find("aac") != std::string::npos);
    me_timeline_destroy(tl);
}

TEST_CASE("audio clip inside a video track is rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"audio","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("must match parent track.kind") != std::string::npos);
}

TEST_CASE("video clip inside an audio track is rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"audio","uri":"file:///tmp/x.wav"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("gainDb on a video clip is rejected as ME_E_PARSE") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra = R"("gainDb":{"static":0.0},)"});
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("gainDb") != std::string::npos);
    CHECK(std::string{err}.find("not valid on video clip") != std::string::npos);
}

TEST_CASE("transform on an audio clip is rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"audio","uri":"file:///tmp/x.wav"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "sourceRange":{"start":{"num":0,"den":48000},"duration":{"num":48000,"den":48000}},
           "transform":{"opacity":{"static":0.5}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("transform") != std::string::npos);
    CHECK(std::string{err}.find("audio clip") != std::string::npos);
}
