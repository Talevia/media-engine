/*
 * test_timeline_schema_clip_attributes — extracted from
 * test_timeline_schema.cpp when that file was split by schema
 * section (debt-split-test-timeline-schema-cpp). Shared fixtures
 * live in timeline_schema_fixtures.hpp.
 */
#include "timeline_schema_fixtures.hpp"

#include <string>
#include <string_view>

using me::tests::schema::load;
using me::tests::schema::EngineFixture;

/* --- Effects (color / blur / lut / enabled / chain) --- */
TEST_CASE("effects: color kind parses with brightness/contrast/saturation") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"id":"e1","kind":"color","params":{"brightness":0.5,"contrast":1.2,"saturation":0.8}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->tl.clips.size() == 1);
    const auto& effs = tl->tl.clips[0].effects;
    REQUIRE(effs.size() == 1);
    CHECK(effs[0].id == "e1");
    CHECK(effs[0].kind == me::EffectKind::Color);
    CHECK(effs[0].enabled == true);
    const auto* cp = std::get_if<me::ColorEffectParams>(&effs[0].params);
    REQUIRE(cp != nullptr);
    CHECK(cp->brightness == doctest::Approx(0.5));
    CHECK(cp->contrast   == doctest::Approx(1.2));
    CHECK(cp->saturation == doctest::Approx(0.8));
    me_timeline_destroy(tl);
}

TEST_CASE("effects: color with only brightness uses defaults for missing params") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"color","params":{"brightness":-0.3}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto* cp = std::get_if<me::ColorEffectParams>(&tl->tl.clips[0].effects[0].params);
    REQUIRE(cp != nullptr);
    CHECK(cp->brightness == doctest::Approx(-0.3));
    CHECK(cp->contrast   == doctest::Approx(1.0));    /* default identity */
    CHECK(cp->saturation == doctest::Approx(1.0));    /* default identity */
    me_timeline_destroy(tl);
}

TEST_CASE("effects: blur kind parses with radius") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"blur","params":{"radius":4.5}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto& e = tl->tl.clips[0].effects[0];
    CHECK(e.kind == me::EffectKind::Blur);
    const auto* bp = std::get_if<me::BlurEffectParams>(&e.params);
    REQUIRE(bp != nullptr);
    CHECK(bp->radius == doctest::Approx(4.5));
    me_timeline_destroy(tl);
}

TEST_CASE("effects: lut kind parses with lutPath") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"lut","params":{"lutPath":"luts/film.cube"}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto& e = tl->tl.clips[0].effects[0];
    CHECK(e.kind == me::EffectKind::Lut);
    const auto* lp = std::get_if<me::LutEffectParams>(&e.params);
    REQUIRE(lp != nullptr);
    CHECK(lp->path == "luts/film.cube");
    me_timeline_destroy(tl);
}

TEST_CASE("effects: enabled=false is preserved") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"color","enabled":false,"params":{}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    CHECK(tl->tl.clips[0].effects[0].enabled == false);
    me_timeline_destroy(tl);
}

TEST_CASE("effects: multi-effect chain preserves order") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[
            {"kind":"color","params":{"brightness":0.1}},
            {"kind":"blur","params":{"radius":2}},
            {"kind":"lut","params":{"lutPath":"a.cube"}}
        ],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto& effs = tl->tl.clips[0].effects;
    REQUIRE(effs.size() == 3);
    CHECK(effs[0].kind == me::EffectKind::Color);
    CHECK(effs[1].kind == me::EffectKind::Blur);
    CHECK(effs[2].kind == me::EffectKind::Lut);
    me_timeline_destroy(tl);
}

TEST_CASE("effects: unknown kind rejected with ME_E_UNSUPPORTED") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"chromaticAberration","params":{}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_UNSUPPORTED);
    CHECK(tl == nullptr);
}

TEST_CASE("effects: blur without required radius rejected with ME_E_PARSE") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"blur","params":{}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("effects: lut without required lutPath rejected with ME_E_PARSE") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"lut","params":{}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("effects: missing params object rejected with ME_E_PARSE") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"color"}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

/* --- Asset colorSpace + Clip transform --- */
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

/* --- M10 HDR: formal-name aliases + cross-axis combo validation --- */

TEST_CASE("colorSpace.transfer accepts 'smpte2084' as alias for PQ") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"bt2020","transfer":"smpte2084",)"
                            R"("matrix":"bt2020nc","range":"limited"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& cs = tl->tl.assets.at("a1").color_space;
    REQUIRE(cs.has_value());
    CHECK(cs->transfer == me::ColorSpace::Transfer::PQ);
    me_timeline_destroy(tl);
}

TEST_CASE("colorSpace.transfer accepts 'arib-std-b67' as alias for HLG") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"bt2020","transfer":"arib-std-b67",)"
                            R"("matrix":"bt2020nc","range":"full"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& cs = tl->tl.assets.at("a1").color_space;
    REQUIRE(cs.has_value());
    CHECK(cs->transfer == me::ColorSpace::Transfer::HLG);
    me_timeline_destroy(tl);
}

TEST_CASE("colorSpace: PQ transfer with bt709 primaries rejected") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"bt709","transfer":"pq"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string_view{err}.find("transfer") != std::string_view::npos);
    CHECK(std::string_view{err}.find("bt2020") != std::string_view::npos);
}

TEST_CASE("colorSpace: HLG transfer (via formal alias) with p3-d65 primaries rejected") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"p3-d65","transfer":"arib-std-b67"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("colorSpace: bt2020nc matrix with bt709 primaries rejected") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"bt709","matrix":"bt2020nc"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string_view{err}.find("bt2020nc") != std::string_view::npos);
}

TEST_CASE("colorSpace: identity matrix with limited range rejected") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"matrix":"identity","range":"limited"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, b.build(), &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
    const char* err = me_engine_last_error(f.eng);
    REQUIRE(err != nullptr);
    CHECK(std::string_view{err}.find("identity") != std::string_view::npos);
}

TEST_CASE("colorSpace: PQ transfer alone (primaries unspecified) accepted") {
    /* Partial specification — host trusts container metadata for the
     * unspecified axis. Validator must not synthesise an
     * inconsistency here. */
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"transfer":"pq"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    const auto& cs = tl->tl.assets.at("a1").color_space;
    REQUIRE(cs.has_value());
    CHECK(cs->transfer  == me::ColorSpace::Transfer::PQ);
    CHECK(cs->primaries == me::ColorSpace::Primaries::Unspecified);
    me_timeline_destroy(tl);
}

TEST_CASE("colorSpace: HLG full HDR10-style descriptor accepted") {
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"bt2020","transfer":"hlg",)"
                            R"("matrix":"bt2020nc","range":"limited"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    me_timeline_destroy(tl);
}

TEST_CASE("colorSpace: bt2020 primaries + bt709 transfer accepted (narrow-gamut SDR)") {
    /* BT.2020 primaries with BT.709 transfer is a legitimate
     * narrow-gamut SDR descriptor; validator must not reject it. */
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{
        .color_space_json = R"({"primaries":"bt2020","transfer":"bt709",)"
                            R"("matrix":"bt2020nc","range":"limited"})",
    });
    b.add_clip(tb::ClipSpec{});

    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    me_timeline_destroy(tl);
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
    /* Fields are AnimatedNumber post-layer-3 migration; evaluate at
     * any T (identity defaults are static so T is irrelevant). */
    const auto tv = t.evaluate_at(me_rational_t{0, 1});
    CHECK(tv.translate_x  == 0.0);
    CHECK(tv.translate_y  == 0.0);
    CHECK(tv.scale_x      == 1.0);
    CHECK(tv.scale_y      == 1.0);
    CHECK(tv.rotation_deg == 0.0);
    CHECK(tv.opacity      == 1.0);
    CHECK(tv.anchor_x     == 0.5);
    CHECK(tv.anchor_y     == 0.5);
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
    const auto tv = t.evaluate_at(me_rational_t{0, 1});
    CHECK(tv.translate_x  == 100.0);
    CHECK(tv.translate_y  == -50.0);
    CHECK(tv.scale_x      == 2.0);
    CHECK(tv.scale_y      == 1.0);   /* unspecified key → identity default retained */
    CHECK(tv.rotation_deg == 45.0);
    CHECK(tv.opacity      == 0.5);
    CHECK(tv.anchor_x     == 0.5);
    me_timeline_destroy(tl);
}

TEST_CASE("clip transform with keyframes (animated) now parses and evaluates per-T") {
    /* Post-layer-3: keyframed transform fields are supported.
     * translateX ramps from 0 (at t=0) to 200 (at t=30/30). At
     * midpoint T=15/30 we expect 100. */
    EngineFixture f;
    tb::TimelineBuilder b;
    b.add_asset(tb::AssetSpec{});
    b.add_clip(tb::ClipSpec{.extra =
        R"("transform":{"translateX":{"keyframes":[)"
        R"({"t":{"num":0,"den":30},"v":0.0,"interp":"linear"},)"
        R"({"t":{"num":30,"den":30},"v":200.0,"interp":"linear"})"
        R"(]}},)"});
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, b.build(), &tl) == ME_OK);
    REQUIRE(tl->tl.clips.size() == 1);
    REQUIRE(tl->tl.clips[0].transform.has_value());
    const auto& t = *tl->tl.clips[0].transform;
    /* Static defaults preserved for unspecified fields. */
    CHECK(t.evaluate_at(me_rational_t{0, 30}).opacity == 1.0);
    /* Linear interp pinned: at 0 → 0; at 15/30 → 100; at 30/30 → 200. */
    CHECK(t.evaluate_at(me_rational_t{0, 30}).translate_x  == doctest::Approx(0.0));
    CHECK(t.evaluate_at(me_rational_t{15, 30}).translate_x == doctest::Approx(100.0));
    CHECK(t.evaluate_at(me_rational_t{30, 30}).translate_x == doctest::Approx(200.0));
    me_timeline_destroy(tl);
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
 * `kind: audio` tracks with `type: audio` clips and optional animated
 * `gainDb` ({"static":v} or {"keyframes":[...]}); AudioMixer evaluates
 * per emitted frame.
 * ======================================================================== */

/* --- Text clip params --- */
TEST_CASE("text clip: minimal positive path parses with defaults") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"text","clips":[
            {"id":"c0","type":"text",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}},
             "textParams":{"content":"Hello"}}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    REQUIRE(tl->tl.clips.size() == 1);
    const auto& c = tl->tl.clips[0];
    CHECK(c.type == me::ClipType::Text);
    REQUIRE(c.text_params.has_value());
    CHECK(c.text_params->content == "Hello");
    /* Default color is opaque white; AnimatedColor post-cycle 40.
     * Check the static RGBA matches {0xFF, 0xFF, 0xFF, 0xFF}. */
    REQUIRE(c.text_params->color.static_value.has_value());
    CHECK((*c.text_params->color.static_value)[0] == 0xFF);
    CHECK((*c.text_params->color.static_value)[1] == 0xFF);
    CHECK((*c.text_params->color.static_value)[2] == 0xFF);
    CHECK((*c.text_params->color.static_value)[3] == 0xFF);
    CHECK(c.text_params->font_family.empty());
    me_timeline_destroy(tl);
}

TEST_CASE("text clip: full fields round-trip") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"text","clips":[
            {"id":"c0","type":"text",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}},
             "textParams":{
               "content":"你好 👋",
               "color":"#FF8800",
               "fontFamily":"Noto Sans SC",
               "fontSize":{"static":72},
               "x":{"static":100},
               "y":{"static":200}
             }}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto& tp = tl->tl.clips[0].text_params;
    REQUIRE(tp.has_value());
    CHECK(tp->content == "你好 👋");
    REQUIRE(tp->color.static_value.has_value());
    CHECK((*tp->color.static_value)[0] == 0xFF);
    CHECK((*tp->color.static_value)[1] == 0x88);
    CHECK((*tp->color.static_value)[2] == 0x00);
    CHECK((*tp->color.static_value)[3] == 0xFF);  // default alpha
    CHECK(tp->font_family == "Noto Sans SC");
    CHECK(tp->font_size.evaluate_at({0, 1}) == doctest::Approx(72.0));
    CHECK(tp->x.evaluate_at({0, 1}) == doctest::Approx(100.0));
    CHECK(tp->y.evaluate_at({0, 1}) == doctest::Approx(200.0));
    me_timeline_destroy(tl);
}

TEST_CASE("text clip: missing content rejected") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"text","clips":[
            {"id":"c0","type":"text",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}},
             "textParams":{}}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("text clip: bad color format rejected") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"text","clips":[
            {"id":"c0","type":"text",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}},
             "textParams":{"content":"hi","color":"orange"}}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
}

TEST_CASE("text clip: textParams on non-text clip is rejected") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("textParams":{"content":"nope"},)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
}

TEST_CASE("text clip: missing textParams on text clip rejected") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"text","clips":[
            {"id":"c0","type":"text",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}}}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
}

TEST_CASE("text clip: animated fontSize keyframes round-trip") {
    EngineFixture f;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":30,"den":1},
      "resolution": {"width":1920,"height":1080},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{
        "id":"main",
        "duration":{"num":2,"den":1},
        "tracks":[{
          "id":"t0","kind":"text","clips":[
            {"id":"c0","type":"text",
             "timeRange":{"start":{"num":0,"den":1},"duration":{"num":2,"den":1}},
             "textParams":{
               "content":"pulse",
               "fontSize":{"keyframes":[
                 {"t":{"num":0,"den":1},"v":24,"interp":"linear"},
                 {"t":{"num":1,"den":1},"v":96,"interp":"linear"}
               ]}
             }}
          ]}]
      }],
      "output": {"compositionId":"main"}
    })";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto& fs = tl->tl.clips[0].text_params->font_size;
    /* Midpoint linear interp: (24 + 96) / 2 = 60. */
    CHECK(fs.evaluate_at({1, 2}) == doctest::Approx(60.0));
    me_timeline_destroy(tl);
}
