/*
 * test_timeline_schema_effects — extracted from
 * test_timeline_schema_clip_attributes.cpp when that file crossed
 * 700 lines (`debt-split-test-timeline-schema-clip-attributes-cpp`).
 * Covers TIMELINE_SCHEMA.md §Effect parsing: positive paths for the
 * three registered effect kinds (color / blur / lut), `enabled` +
 * multi-effect chain ordering, plus parse-time rejections (unknown
 * kind, missing required params, missing `params` object).
 *
 * Shared fixtures via timeline_schema_fixtures.hpp; uses tb::
 * TimelineBuilder for terse JSON construction.
 */
#include "timeline_schema_fixtures.hpp"

#include <string>

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

/* --- M10 tonemap effect (HDR → SDR) --- */
TEST_CASE("effects: tonemap with default params (Hable / 100 nits)") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"tonemap","params":{}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl->tl.clips.size() == 1);
    const auto& effs = tl->tl.clips[0].effects;
    REQUIRE(effs.size() == 1);
    CHECK(effs[0].kind == me::EffectKind::Tonemap);
    const auto* tp = std::get_if<me::TonemapEffectParams>(&effs[0].params);
    REQUIRE(tp != nullptr);
    CHECK(tp->algo == me::TonemapEffectParams::Algo::Hable);
    CHECK(tp->target_nits == doctest::Approx(100.0));
    me_timeline_destroy(tl);
}

TEST_CASE("effects: tonemap with explicit ACES algo + 400 nits") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"tonemap","params":{"algo":"aces","targetNits":400}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto* tp = std::get_if<me::TonemapEffectParams>(
        &tl->tl.clips[0].effects[0].params);
    REQUIRE(tp != nullptr);
    CHECK(tp->algo == me::TonemapEffectParams::Algo::ACES);
    CHECK(tp->target_nits == doctest::Approx(400.0));
    me_timeline_destroy(tl);
}

TEST_CASE("effects: tonemap accepts reinhard algo") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"tonemap","params":{"algo":"reinhard"}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto* tp = std::get_if<me::TonemapEffectParams>(
        &tl->tl.clips[0].effects[0].params);
    REQUIRE(tp != nullptr);
    CHECK(tp->algo == me::TonemapEffectParams::Algo::Reinhard);
    me_timeline_destroy(tl);
}

TEST_CASE("effects: tonemap unknown algo rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"tonemap","params":{"algo":"hejl"}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("effects: tonemap targetNits <= 0 rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"tonemap","params":{"targetNits":-50}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

/* --- M10 inverse_tonemap effect (SDR → HDR; registered, deferred impl) --- */
TEST_CASE("effects: inverse_tonemap with default params (Linear / 1000 nits)") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"inverse_tonemap","params":{}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    REQUIRE(tl->tl.clips.size() == 1);
    const auto& effs = tl->tl.clips[0].effects;
    REQUIRE(effs.size() == 1);
    CHECK(effs[0].kind == me::EffectKind::InverseTonemap);
    const auto* ip = std::get_if<me::InverseTonemapEffectParams>(&effs[0].params);
    REQUIRE(ip != nullptr);
    CHECK(ip->algo == me::InverseTonemapEffectParams::Algo::Linear);
    CHECK(ip->target_peak_nits == doctest::Approx(1000.0));
    me_timeline_destroy(tl);
}

TEST_CASE("effects: inverse_tonemap with explicit Hable + 600 peak nits") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"inverse_tonemap","params":{"algo":"hable","targetPeakNits":600}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto* ip = std::get_if<me::InverseTonemapEffectParams>(
        &tl->tl.clips[0].effects[0].params);
    REQUIRE(ip != nullptr);
    CHECK(ip->algo == me::InverseTonemapEffectParams::Algo::Hable);
    CHECK(ip->target_peak_nits == doctest::Approx(600.0));
    me_timeline_destroy(tl);
}

TEST_CASE("effects: inverse_tonemap unknown algo rejected as ME_E_PARSE") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"inverse_tonemap","params":{"algo":"deephdr"}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}

TEST_CASE("effects: inverse_tonemap targetPeakNits <= 0 rejected") {
    EngineFixture f;
    const std::string j = tb::minimal_video_clip()
        .with_clip_extra(R"("effects":[{"kind":"inverse_tonemap","params":{"targetPeakNits":0}}],)")
        .build();
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
    CHECK(tl == nullptr);
}
