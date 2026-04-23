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
