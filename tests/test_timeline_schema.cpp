/*
 * test_timeline_schema — core schema cases. Extracted sections:
 *   - audio / gainDb                   → test_timeline_schema_audio.cpp
 *   - effects / transform / text clip  → test_timeline_schema_clip_attributes.cpp
 *   - transitions                      → test_timeline_schema_transitions.cpp
 * Shared fixtures: timeline_schema_fixtures.hpp.
 *
 * This file retains: schemaVersion / malformed JSON / asset
 * reference / multi-clip contiguity / multi-track IR + render
 * routing / unknown-kind tracks / duplicate ids / general error
 * handling.
 */
#include "timeline_schema_fixtures.hpp"

#include <string>
#include <string_view>

using me::tests::schema::load;
using me::tests::schema::EngineFixture;
TEST_CASE("valid single-clip timeline loads") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string j = tb::minimal_video_clip().build();
    CHECK(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl != nullptr);

    int w = 0, h = 0;
    me_timeline_resolution(tl, &w, &h);
    CHECK(w == 1920);
    CHECK(h == 1080);

    me_rational_t fr = me_timeline_frame_rate(tl);
    CHECK(fr.num == 30);
    CHECK(fr.den == 1);

    me_timeline_destroy(tl);
}

TEST_CASE("schemaVersion != 1 is rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip().schema_version(2).build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("malformed JSON is rejected as ME_E_PARSE") {
    EngineFixture f;
    std::string_view bad = R"({ not valid json )";
    me_timeline_t* tl = nullptr;
    CHECK(me_timeline_load_json(f.eng, bad.data(), bad.size(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("multi-clip single-track with contiguous time ranges loads") {
    EngineFixture f;
    /* Two clips, each 60/30 = 2s of source, concatenated back-to-back in
     * the composition (clip 2 starts at 60/30, ends at 120/30 → 4s total). */
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.clip_id = "c1"});
    b.add_clip(tb::ClipSpec{
        .clip_id = "c2",
        .time_start_num = 60,  /* starts where clip 1 ended */
    });

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_OK);
    REQUIRE(tl != nullptr);

    me_rational_t dur = me_timeline_duration(tl);
    CHECK(static_cast<double>(dur.num) / dur.den == doctest::Approx(4.0));
    me_timeline_destroy(tl);
}

TEST_CASE("phase-1 rejects non-contiguous clips (gap/overlap)") {
    EngineFixture f;
    /* Both clips declare timeRange.start=0 — second clip's start should
     * equal first's duration (60/30), not 0. Loader must catch overlap. */
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.clip_id = "c1"});
    b.add_clip(tb::ClipSpec{.clip_id = "c2"});  /* time_start defaults to 0 → overlap */

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
}

TEST_CASE("missing schemaVersion is rejected as ME_E_PARSE") {
    /* JSON without the `schemaVersion` field reads as 0 via the loader's
     * `doc.value("schemaVersion", 0)` default, which then fails the
     * "schemaVersion must be 1" require. Pinned separately from the
     * explicit schemaVersion=2 case so removing the default value code
     * path doesn't silently change the behaviour. */
    EngineFixture f;
    const std::string j = R"({
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
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
    CHECK(std::string{me_engine_last_error(f.eng)}.find("schemaVersion") != std::string::npos);
}

TEST_CASE("empty track.clips is rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    CHECK(std::string{me_engine_last_error(f.eng)}.find("at least one clip") != std::string::npos);
}

TEST_CASE("unknown assetId reference is rejected as ME_E_PARSE") {
    /* Clip references assetId="a-missing" which the assets array doesn't
     * define. Loader's per-clip `tl.assets.find(asset_id)` must fail
     * before any further processing. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a-missing",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    CHECK(std::string{me_engine_last_error(f.eng)}.find("unknown asset") != std::string::npos);
}

TEST_CASE("timeRange.duration.den == 0 is rejected as ME_E_PARSE") {
    /* Rational with den=0 would divide-by-zero downstream. Loader's
     * as_rational helper catches this at parse time. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":0}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    CHECK(std::string{me_engine_last_error(f.eng)}.find("den must be > 0") != std::string::npos);
}

TEST_CASE("output.compositionId pointing at unknown composition is rejected as ME_E_PARSE") {
    /* The output block names a composition that doesn't exist in
     * compositions[]. Loader has to catch this before resolving tracks/
     * clips — without this check the next line derefs a null ptr. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"not-main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    CHECK(std::string{me_engine_last_error(f.eng)}.find("unknown composition") != std::string::npos);
}

TEST_CASE("multi-track timeline loads into IR with track_id stamped on each clip") {
    /* Loader accepts N tracks and flattens clips into Timeline::clips
     * with track_id back-references. Tracks metadata is preserved in
     * Timeline::tracks in JSON declaration order. The Exporter still
     * rejects multi-track (see multi-track-compose-kernel), but IR
     * consumers (future compose kernel, segmentation variants) can
     * see the full structure. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [
        {"id":"a1","kind":"video","uri":"file:///tmp/input.mp4"}
      ],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]},
        {"id":"v1","kind":"video","enabled":false,"clips":[
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":90,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":90,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    CHECK(tl->tl.tracks.size() == 2);
    CHECK(tl->tl.tracks[0].id == "v0");
    CHECK(tl->tl.tracks[0].enabled == true);
    CHECK(tl->tl.tracks[1].id == "v1");
    CHECK(tl->tl.tracks[1].enabled == false);
    REQUIRE(tl->tl.clips.size() == 2);
    CHECK(tl->tl.clips[0].track_id == "v0");
    CHECK(tl->tl.clips[1].track_id == "v1");
    /* Timeline duration is max across tracks: v1 is 3s, v0 is 2s. */
    CHECK(tl->tl.duration.num == 90);
    CHECK(tl->tl.duration.den == 30);
    me_timeline_destroy(tl);
}

TEST_CASE("multi-track + passthrough codec is rejected synchronously by compose factory") {
    /* Compose path requires re-encode (can't stream-copy composite two
     * video streams into one). The make_compose_sink factory rejects
     * passthrough codec combinations synchronously, surfacing through
     * me_render_start. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [
        {"id":"a1","kind":"video","uri":"file:///tmp/input.mp4"}
      ],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]},
        {"id":"v1","kind":"video","clips":[
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);

    me_output_spec_t spec{};
    spec.path        = "/tmp/me-multi-track-reject.mp4";
    spec.container   = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    me_render_job_t* job = nullptr;
    CHECK(me_render_start(f.eng, tl, &spec, nullptr, nullptr, &job) == ME_E_UNSUPPORTED);
    CHECK(job == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("compose path") != std::string::npos);
    me_timeline_destroy(tl);
}

TEST_CASE("multi-track + h264/aac timeline renders (bottom track only, pending full compose)") {
    /* ComposeSink currently delegates to reencode_mux using only
     * tracks[0]'s clips (phase-1: full alpha_over compose lands in
     * multi-track-compose-actual-composite). File references are
     * bogus so the render hits an I/O error eventually, but the
     * pipeline construction + routing must succeed — me_render_start
     * returns ME_OK and the failure reported by me_render_wait is an
     * I/O error (fake URI), NOT the old "compose loop not implemented"
     * UNSUPPORTED. Pins that the ComposeSink now dispatches to real
     * encoder/mux machinery. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [
        {"id":"a1","kind":"video","uri":"file:///tmp/me-nonexistent.mp4"}
      ],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]},
        {"id":"v1","kind":"video","clips":[
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);

    me_output_spec_t spec{};
    spec.path        = "/tmp/me-multi-track-bottom-only.mp4";
    spec.container   = "mp4";
    spec.video_codec = "h264";
    spec.audio_codec = "aac";

    me_render_job_t* job = nullptr;
    REQUIRE(me_render_start(f.eng, tl, &spec, nullptr, nullptr, &job) == ME_OK);
    REQUIRE(job != nullptr);
    const me_status_t wait_s = me_render_wait(job);
    /* Either ME_OK (fake URI could in principle "open" depending on host
     * filesystem state) or ME_E_IO / ME_E_INTERNAL — what we care about
     * is that the old "per-frame compose loop not yet implemented"
     * UNSUPPORTED path is gone. Check err does NOT mention the old
     * stub message. */
    if (wait_s != ME_OK) {
        const char* err = me_engine_last_error(f.eng);
        if (err) {
            CHECK(std::string{err}.find("per-frame compose loop not yet implemented")
                  == std::string::npos);
        }
    }
    me_render_job_destroy(job);
    me_timeline_destroy(tl);
}

TEST_CASE("duplicate track ids are rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/input.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]},
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c2","assetId":"a1",
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
    CHECK(std::string{err}.find("duplicate track id") != std::string::npos);
}

TEST_CASE("within-track gap is still rejected with per-track error message") {
    /* Relaxing multi-track doesn't relax within-track contiguity —
     * each track independently must be contiguous in phase-1. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/input.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}},
          {"type":"video","id":"c2","assetId":"a1",
           "timeRange":{"start":{"num":120,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    const std::string s{err};
    CHECK(s.find("within this track") != std::string::npos);
    CHECK(s.find("track[0]") != std::string::npos);
}

TEST_CASE("single-track timeline stamps track_id on clip (backward-compat)") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string j = tb::minimal_video_clip().build();
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    CHECK(tl->tl.tracks.size() == 1);
    CHECK(tl->tl.tracks[0].id == "v0");
    REQUIRE(tl->tl.clips.size() == 1);
    CHECK(tl->tl.clips[0].track_id == "v0");
    me_timeline_destroy(tl);
}

/* --- General loader error handling --- */
TEST_CASE("load_json(NULL engine) returns ME_E_INVALID_ARG") {
    me_timeline_t* tl = nullptr;
    const std::string j = tb::minimal_video_clip().build();
    CHECK(me_timeline_load_json(nullptr, j.data(), j.size(), &tl) == ME_E_INVALID_ARG);
    CHECK(tl == nullptr);
}

TEST_CASE("load_json populates last_error on rejection") {
    EngineFixture f;
    std::string_view bad = R"({"schemaVersion":2})";
    me_timeline_t* tl = nullptr;
    CHECK(me_timeline_load_json(f.eng, bad.data(), bad.size(), &tl) == ME_E_PARSE);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string_view{err}.find("schemaVersion") != std::string_view::npos);
}

/* --- Unknown track.kind --- */
TEST_CASE("unknown track.kind is rejected as ME_E_UNSUPPORTED") {
    /* Track kind `"unicorn"` is genuinely unknown — video / audio /
     * text / subtitle are all accepted today (subtitle lands in
     * cycle 30's compose-sink-subtitle-track-wire). The loader's
     * error enumerates the accepted set, which we spot-check as
     * substrings so future kind additions don't break this
     * assertion. */
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"t0","kind":"unicorn","clips":[
          {"type":"unicorn","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    const std::string e{err};
    CHECK(e.find("unicorn")   != std::string::npos);
    CHECK(e.find("only")      != std::string::npos);
    CHECK(e.find("video")     != std::string::npos);
    CHECK(e.find("subtitle")  != std::string::npos);
}

/* ========================================================================
 * Transitions — cross-dissolve-transition schema+IR scope. Loader parses
 * `track.transitions[]` with adjacency + duration-bound validation;
 * Exporter rejects any non-empty transitions until cross-dissolve-kernel
 * lands.
 * ======================================================================== */

/* ========================================================================
 * Text clips — ClipType::Text + TextClipParams. Text tracks carry
 * synthetic clips (no source media); loader validates required content
 * + optional color/fontFamily/fontSize/x/y.
 * ======================================================================== */

/* --- Duplicate clip ids --- */
TEST_CASE("duplicate clip ids within a track rejected as ME_E_PARSE") {
    /* Loader now tracks clip ids for transition resolution; duplicate
     * ids within a track would break that lookup, so enforce
     * uniqueness at load time. */
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
          {"type":"video","id":"c1","assetId":"a1",
           "timeRange":{"start":{"num":60,"den":30},"duration":{"num":60,"den":30}},
           "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("duplicate clip id") != std::string::npos);
}
