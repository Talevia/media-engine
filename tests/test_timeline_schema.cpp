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

/* ========================================================================
 * Audio tracks — audio-mix-two-track schema+IR scope. Loader accepts
 * `kind: audio` tracks with `type: audio` clips and optional static
 * `gainDb`; Exporter rejects audio tracks until audio-mix-kernel lands.
 * ======================================================================== */

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
    CHECK(*tl->tl.clips[0].gain_db == -6.0);
    CHECK_FALSE(tl->tl.clips[0].transform.has_value());
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

TEST_CASE("unknown track.kind is rejected as ME_E_UNSUPPORTED") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/x.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"t0","kind":"subtitle","clips":[
          {"type":"video","id":"c1","assetId":"a1",
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
    CHECK(std::string{err}.find("only 'video' and 'audio'") != std::string::npos);
}

/* ========================================================================
 * Transitions — cross-dissolve-transition schema+IR scope. Loader parses
 * `track.transitions[]` with adjacency + duration-bound validation;
 * Exporter rejects any non-empty transitions until cross-dissolve-kernel
 * lands.
 * ======================================================================== */

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
