#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"
#include "timeline/timeline_impl.hpp"

#include <string>
#include <string_view>

namespace {

namespace tb = me::tests::tb;

me_status_t load(me_engine_t* eng, const std::string& json, me_timeline_t** out) {
    return me_timeline_load_json(eng, json.data(), json.size(), out);
}

struct EngineFixture {
    me_engine_t* eng = nullptr;
    EngineFixture()  { REQUIRE(me_engine_create(nullptr, &eng) == ME_OK); }
    ~EngineFixture() { me_engine_destroy(eng); }
};

}  // namespace

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

TEST_CASE("phase-1 rejects multi-track timeline as ME_E_UNSUPPORTED") {
    /* Loader's single-track enforcement is the tripwire that keeps the
     * Exporter / OutputSink path from silently dropping every track
     * after the first. TimelineBuilder is single-track by design, so
     * this negative case builds the JSON inline — cheaper than
     * extending the builder for a gap that gets lifted once
     * `multi-track-video-compose` lands. */
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
    CHECK(load(f.eng, j, &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);

    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string{err}.find("exactly one track") != std::string::npos);
}

TEST_CASE("phase-1 rejects clip.effects") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"blur","params":{"radius":{"num":1,"den":1}}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
}

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

TEST_CASE("asset.colorSpace parsed into me::Asset::color_space") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"bt2020","transfer":"pq",)"
                            R"("matrix":"bt2020nc","range":"full"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    REQUIRE(tl != nullptr);

    const auto& assets = tl->tl.assets;
    REQUIRE(assets.count("a1") == 1);
    const auto& cs = assets.at("a1").color_space;
    REQUIRE(cs.has_value());
    CHECK(cs->primaries == me::ColorSpace::Primaries::BT2020);
    CHECK(cs->transfer  == me::ColorSpace::Transfer::PQ);
    CHECK(cs->matrix    == me::ColorSpace::Matrix::BT2020NC);
    CHECK(cs->range     == me::ColorSpace::Range::Full);

    me_timeline_destroy(tl);
}

TEST_CASE("asset without colorSpace leaves color_space as nullopt") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, tb::minimal_video_clip().build(), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    CHECK_FALSE(tl->tl.assets.at("a1").color_space.has_value());
    me_timeline_destroy(tl);
}

TEST_CASE("asset.colorSpace with unknown enum is rejected as ME_E_PARSE") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"xyz-wide"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string_view{err}.find("primaries") != std::string_view::npos);
}

/* ========================================================================
 * clip.transform (static-only) — M2 Transform (静态) exit criterion
 * scaffolding. Schema stored in IR; compose path not wired this cycle
 * (see multi-track-video-compose backlog item).
 * ======================================================================== */

TEST_CASE("clip absent transform → optional nullopt") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string j = tb::minimal_video_clip().build();
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl->tl.clips.size() == 1);
    CHECK_FALSE(tl->tl.clips[0].transform.has_value());
    me_timeline_destroy(tl);
}

TEST_CASE("clip transform empty object → populated with identity defaults") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra = R"("transform":{},)"});
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    REQUIRE(tl->tl.clips.size() == 1);
    REQUIRE(tl->tl.clips[0].transform.has_value());
    const auto& t = *tl->tl.clips[0].transform;
    CHECK(t.translate_x  == 0.0);
    CHECK(t.translate_y  == 0.0);
    CHECK(t.scale_x      == 1.0);
    CHECK(t.scale_y      == 1.0);
    CHECK(t.rotation_deg == 0.0);
    CHECK(t.opacity      == 1.0);
    CHECK(t.anchor_x     == 0.5);
    CHECK(t.anchor_y     == 0.5);
    me_timeline_destroy(tl);
}

TEST_CASE("clip transform with static translateX / opacity / rotationDeg parses to IR") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra =
        R"("transform":{)"
        R"("translateX":{"static":100},)"
        R"("translateY":{"static":-50},)"
        R"("scaleX":{"static":2.0},)"
        R"("rotationDeg":{"static":45},)"
        R"("opacity":{"static":0.5})"
        R"(},)"});
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    REQUIRE(tl->tl.clips.size() == 1);
    REQUIRE(tl->tl.clips[0].transform.has_value());
    const auto& t = *tl->tl.clips[0].transform;
    CHECK(t.translate_x  == 100.0);
    CHECK(t.translate_y  == -50.0);
    CHECK(t.scale_x      == 2.0);
    CHECK(t.scale_y      == 1.0);   /* unspecified key → identity default retained */
    CHECK(t.rotation_deg == 45.0);
    CHECK(t.opacity      == 0.5);
    CHECK(t.anchor_x     == 0.5);
    me_timeline_destroy(tl);
}

TEST_CASE("clip transform with keyframes (animated) rejected as ME_E_UNSUPPORTED") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra =
        R"("transform":{"translateX":{"keyframes":[{"t":0,"v":0}]}},)"});
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    const std::string_view s{err};
    CHECK(s.find("animated") != std::string_view::npos);
    CHECK(s.find("keyframes") != std::string_view::npos);
}

TEST_CASE("clip transform opacity out of [0,1] rejected as ME_E_PARSE") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra =
        R"("transform":{"opacity":{"static":2.0}},)"});
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string_view{err}.find("opacity") != std::string_view::npos);
}

TEST_CASE("clip transform unknown key rejected as ME_E_PARSE") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra =
        R"("transform":{"skew":{"static":10}},)"});
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    const std::string_view s{err};
    CHECK(s.find("unknown transform key") != std::string_view::npos);
    CHECK(s.find("skew") != std::string_view::npos);
}

TEST_CASE("clip transform static must be a number — object-shaped value rejected") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra =
        R"("transform":{"scaleX":{"static":{"nested":1}}},)"});
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("clip transform missing static key rejected as ME_E_PARSE") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra =
        R"("transform":{"translateX":{}},)"});
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}
