/*
 * test_face_mosaic_stub — pins the registered-but-deferred contract
 * for `me::compose::apply_face_mosaic_inplace` (M11
 * ml-effect-face-mosaic-stub). Mirrors test_face_sticker_stub's
 * shape — argument-validation prologue + UNSUPPORTED short-circuit
 * + JSON loader round-trip.
 */
#include <doctest/doctest.h>

#include "compose/face_mosaic_kernel.hpp"
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

me::FaceMosaicEffectParams valid_params() {
    me::FaceMosaicEffectParams p;
    p.landmark_asset_id = "ml1";
    p.block_size_px     = 16;
    p.kind              = me::FaceMosaicEffectParams::Kind::Pixelate;
    return p;
}

}  // namespace

TEST_CASE("face_mosaic stub: null buffer rejected with INVALID_ARG") {
    auto p = valid_params();
    CHECK(me::compose::apply_face_mosaic_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic stub: non-positive dimensions rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    CHECK(me::compose::apply_face_mosaic_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_face_mosaic_inplace(buf.data(), 16, -1, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic stub: stride < width*4 rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    CHECK(me::compose::apply_face_mosaic_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic stub: empty landmark_asset_id rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    p.landmark_asset_id.clear();
    CHECK(me::compose::apply_face_mosaic_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic stub: non-positive block_size_px rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    p.block_size_px = 0;
    CHECK(me::compose::apply_face_mosaic_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.block_size_px = -8;
    CHECK(me::compose::apply_face_mosaic_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_mosaic stub: valid input returns ME_E_UNSUPPORTED") {
    auto buf = make_known(8, 8);
    auto p = valid_params();
    CHECK(me::compose::apply_face_mosaic_inplace(buf.data(), 8, 8, 32, p)
          == ME_E_UNSUPPORTED);
}

TEST_CASE("face_mosaic stub: buffer is NOT mutated") {
    auto buf = make_known(8, 8);
    const auto snapshot = buf;
    auto p = valid_params();
    REQUIRE(me::compose::apply_face_mosaic_inplace(buf.data(), 8, 8, 32, p)
            == ME_E_UNSUPPORTED);
    CHECK(buf == snapshot);
}

/* ----------------------------------------- JSON loader round-trip */

namespace {

std::string face_mosaic_timeline(const std::string& effect_body) {
    return std::string{R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"ml1","uri":"file:///tmp/landmarks.bin","type":"landmark",
     "model":{"id":"blazeface","version":"v2","quantization":"fp16"}}
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

TEST_CASE("face_mosaic JSON: full param set parses into IR") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{
            "landmarkAssetId":"ml1",
            "blockSizePx":24,
            "kind":"blur"
        }
    })";
    REQUIRE(load(f.eng, face_mosaic_timeline(body), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& effects = tl->tl.clips[0].effects;
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].kind == me::EffectKind::FaceMosaic);
    const auto* fp = std::get_if<me::FaceMosaicEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark_asset_id == "ml1");
    CHECK(fp->block_size_px == 24);
    CHECK(fp->kind == me::FaceMosaicEffectParams::Kind::Blur);
    me_timeline_destroy(tl);
}

TEST_CASE("face_mosaic JSON: defaults applied when optional fields omitted") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1"}
    })";
    REQUIRE(load(f.eng, face_mosaic_timeline(body), &tl) == ME_OK);
    const auto& fp = std::get<me::FaceMosaicEffectParams>(
        tl->tl.clips[0].effects[0].params);
    CHECK(fp.block_size_px == 16);   /* default */
    CHECK(fp.kind == me::FaceMosaicEffectParams::Kind::Pixelate);
    me_timeline_destroy(tl);
}

TEST_CASE("face_mosaic JSON: missing landmarkAssetId → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{"blockSizePx":16}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body), &tl) == ME_E_PARSE);
}

TEST_CASE("face_mosaic JSON: blockSizePx out of range → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body_zero = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1","blockSizePx":0}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body_zero), &tl) == ME_E_PARSE);
    const std::string body_huge = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1","blockSizePx":99999}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body_huge), &tl) == ME_E_PARSE);
}

TEST_CASE("face_mosaic JSON: unknown kind value → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{"landmarkAssetId":"ml1","kind":"swirl"}
    })";
    CHECK(load(f.eng, face_mosaic_timeline(body), &tl) == ME_E_PARSE);
}
