/*
 * test_face_sticker_stub — pins the registered-but-deferred contract
 * for `me::compose::apply_face_sticker_inplace` (M11
 * ml-effect-face-sticker-stub). Mirrors test_inverse_tonemap_stub's
 * shape — argument-validation prologue + UNSUPPORTED short-circuit
 * + JSON loader round-trip via the schema-fixtures harness.
 *
 * What this suite asserts:
 *   - Argument-shape rejects (null buffer, non-positive dims,
 *     undersized stride, empty landmark_asset_id, empty
 *     sticker_uri) land before the UNSUPPORTED short-circuit.
 *   - Otherwise-valid input returns ME_E_UNSUPPORTED — the
 *     deterministic stub answer.
 *   - The buffer is NOT mutated by the stub.
 *   - JSON loader accepts `kind: "face_sticker"` with all required
 *     fields + populates `FaceStickerEffectParams` correctly.
 *   - JSON loader rejects missing required fields (landmarkAssetId,
 *     stickerUri) with ME_E_PARSE.
 */
#include <doctest/doctest.h>

#include "compose/face_sticker_kernel.hpp"
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

me::FaceStickerEffectParams valid_params() {
    me::FaceStickerEffectParams p;
    p.landmark_asset_id = "ml1";
    p.sticker_uri       = "file:///tmp/star.png";
    return p;
}

}  // namespace

TEST_CASE("face_sticker stub: null buffer rejected with INVALID_ARG") {
    auto p = valid_params();
    CHECK(me::compose::apply_face_sticker_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker stub: non-positive dimensions rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    CHECK(me::compose::apply_face_sticker_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_face_sticker_inplace(buf.data(), 16, -1, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker stub: stride < width*4 rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    CHECK(me::compose::apply_face_sticker_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker stub: empty landmark_asset_id rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    p.landmark_asset_id.clear();
    CHECK(me::compose::apply_face_sticker_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker stub: empty sticker_uri rejected") {
    auto buf = make_known(16, 16);
    auto p = valid_params();
    p.sticker_uri.clear();
    CHECK(me::compose::apply_face_sticker_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("face_sticker stub: valid input returns ME_E_UNSUPPORTED") {
    auto buf = make_known(8, 8);
    auto p = valid_params();
    CHECK(me::compose::apply_face_sticker_inplace(buf.data(), 8, 8, 32, p)
          == ME_E_UNSUPPORTED);
}

TEST_CASE("face_sticker stub: buffer is NOT mutated") {
    auto buf = make_known(8, 8);
    const auto snapshot = buf;
    auto p = valid_params();
    REQUIRE(me::compose::apply_face_sticker_inplace(buf.data(), 8, 8, 32, p)
            == ME_E_UNSUPPORTED);
    CHECK(buf == snapshot);
}

/* ----------------------------------------- JSON loader round-trip */

namespace {

std::string face_sticker_timeline(const std::string& effect_body) {
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

TEST_CASE("face_sticker JSON: required + optional fields parse into IR") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{
            "landmarkAssetId":"ml1",
            "stickerUri":"file:///tmp/star.png",
            "scaleX":1.5, "scaleY":2.0,
            "offsetX":10.0, "offsetY":-5.0
        }
    })";
    REQUIRE(load(f.eng, face_sticker_timeline(body), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& clips = tl->tl.clips;
    REQUIRE(clips.size() == 1);
    const auto& effects = clips[0].effects;
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].kind == me::EffectKind::FaceSticker);
    const auto* fp = std::get_if<me::FaceStickerEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark_asset_id == "ml1");
    CHECK(fp->sticker_uri       == "file:///tmp/star.png");
    CHECK(fp->scale_x == 1.5);
    CHECK(fp->scale_y == 2.0);
    CHECK(fp->offset_x == 10.0);
    CHECK(fp->offset_y == -5.0);
    me_timeline_destroy(tl);
}

TEST_CASE("face_sticker JSON: defaults applied when optional fields omitted") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{
            "landmarkAssetId":"ml1",
            "stickerUri":"file:///tmp/star.png"
        }
    })";
    REQUIRE(load(f.eng, face_sticker_timeline(body), &tl) == ME_OK);
    const auto& fp = std::get<me::FaceStickerEffectParams>(
        tl->tl.clips[0].effects[0].params);
    CHECK(fp.scale_x == 1.0);
    CHECK(fp.scale_y == 1.0);
    CHECK(fp.offset_x == 0.0);
    CHECK(fp.offset_y == 0.0);
    me_timeline_destroy(tl);
}

TEST_CASE("face_sticker JSON: missing landmarkAssetId → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{"stickerUri":"file:///tmp/star.png"}
    })";
    CHECK(load(f.eng, face_sticker_timeline(body), &tl) == ME_E_PARSE);
}

TEST_CASE("face_sticker JSON: missing stickerUri → ME_E_PARSE") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{"landmarkAssetId":"ml1"}
    })";
    CHECK(load(f.eng, face_sticker_timeline(body), &tl) == ME_E_PARSE);
}
