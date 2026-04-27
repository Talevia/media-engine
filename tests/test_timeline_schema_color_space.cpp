/*
 * test_timeline_schema_color_space — extracted from
 * test_timeline_schema_clip_attributes.cpp when that file crossed
 * 700 lines (`debt-split-test-timeline-schema-clip-attributes-cpp`).
 * Covers TIMELINE_SCHEMA.md §Color: per-asset colorSpace round-trip,
 * the M10 HDR formal-name aliases (smpte2084 / arib-std-b67), and
 * the cross-axis combo validator (R1/R2/R3 in
 * `loader_helpers_primitives.cpp::validate_color_space_combo`).
 *
 * Shared fixtures via timeline_schema_fixtures.hpp.
 */
#include "timeline_schema_fixtures.hpp"

#include <string>
#include <string_view>

using me::tests::schema::load;
using me::tests::schema::EngineFixture;

/* --- Asset colorSpace round-trip --- */
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
