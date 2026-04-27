/*
 * test_body_alpha_key_stub — pins the registered-but-deferred
 * contract for `me::compose::apply_body_alpha_key_inplace` (M11
 * ml-effect-body-alpha-key-stub). Mirrors test_face_mosaic_stub's
 * shape — argument-validation prologue + UNSUPPORTED short-circuit
 * + JSON loader round-trip.
 */
#include <doctest/doctest.h>

#include "compose/body_alpha_key_kernel.hpp"
#include "timeline/timeline_impl.hpp"
#include "timeline_schema_fixtures.hpp"

#include <cstdint>
#include <variant>
#include <vector>

using me::tests::schema::EngineFixture;
using me::tests::schema::load;

namespace {

std::vector<std::uint8_t> make_known(int w, int h) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::uint8_t>(i & 0xff);
    }
    return buf;
}

me::BodyAlphaKeyEffectParams valid_params() {
    me::BodyAlphaKeyEffectParams p;
    p.mask_asset_id     = "mask1";
    p.feather_radius_px = 4;
    p.invert            = false;
    return p;
}

}  // namespace

TEST_CASE("body_alpha_key stub: null buffer rejected with INVALID_ARG") {
    auto p = valid_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key stub: non-positive dimensions rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_body_alpha_key_inplace(buf.data(), 16, -1, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key stub: stride < width*4 rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key stub: empty mask_asset_id rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    p.mask_asset_id.clear();
    CHECK(me::compose::apply_body_alpha_key_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key stub: negative feather_radius_px rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    p.feather_radius_px = -1;
    CHECK(me::compose::apply_body_alpha_key_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("body_alpha_key stub: valid input returns ME_E_UNSUPPORTED") {
    auto buf = make_known(8, 8);
    auto p = valid_params();
    CHECK(me::compose::apply_body_alpha_key_inplace(buf.data(), 8, 8, 32, p)
          == ME_E_UNSUPPORTED);
}

TEST_CASE("body_alpha_key stub: buffer is NOT mutated") {
    auto buf = make_known(8, 8);
    const auto snapshot = buf;
    auto p = valid_params();
    REQUIRE(me::compose::apply_body_alpha_key_inplace(buf.data(), 8, 8, 32, p)
            == ME_E_UNSUPPORTED);
    CHECK(buf == snapshot);
}

/* ----------------------------------------- JSON loader round-trip */

namespace {

std::string body_alpha_key_timeline(const std::string& effect_body) {
    return std::string{R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"mask1","uri":"file:///tmp/mask.bin","type":"mask",
     "model":{"id":"selfie_seg","version":"v3","quantization":"int8"}}
  ],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"v1",
     "effects":[)"} + effect_body + R"(],
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
}
)";
}

}  // namespace

TEST_CASE("body_alpha_key JSON: full param set parses into IR") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{
            "maskAssetId":"mask1",
            "featherRadiusPx":8,
            "invert":true
        }
    })";
    REQUIRE(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& effects = tl->tl.clips[0].effects;
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].kind == me::EffectKind::BodyAlphaKey);
    const auto* bp = std::get_if<me::BodyAlphaKeyEffectParams>(&effects[0].params);
    REQUIRE(bp != nullptr);
    CHECK(bp->mask_asset_id == "mask1");
    CHECK(bp->feather_radius_px == 8);
    CHECK(bp->invert == true);
    me_timeline_destroy(tl);
}

TEST_CASE("body_alpha_key JSON: defaults applied when optional fields omitted") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1"}
    })";
    REQUIRE(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_OK);
    const auto& bp = std::get<me::BodyAlphaKeyEffectParams>(
        tl->tl.clips[0].effects[0].params);
    CHECK(bp.feather_radius_px == 0);   /* default sharp edge */
    CHECK(bp.invert == false);
    me_timeline_destroy(tl);
}

TEST_CASE("body_alpha_key JSON: missing maskAssetId → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{"featherRadiusPx":4}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_E_PARSE);
}

TEST_CASE("body_alpha_key JSON: featherRadiusPx out of range → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body_neg = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1","featherRadiusPx":-3}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body_neg), &tl) == ME_E_PARSE);
    const std::string body_huge = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1","featherRadiusPx":9999}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body_huge), &tl) == ME_E_PARSE);
}

TEST_CASE("body_alpha_key JSON: invert non-boolean → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{"maskAssetId":"mask1","invert":"true"}
    })";
    CHECK(load(f.eng, body_alpha_key_timeline(body), &tl) == ME_E_PARSE);
}
