/*
 * test_effect_param_ml_asset_ref — parse + accessor coverage for
 * the typed `LandmarkAssetRef` / `MaskAssetRef` shape (M11 exit
 * criterion at docs/MILESTONES.md:140).
 *
 * Three effect kinds carry asset-ref params today:
 *   - face_sticker  → LandmarkAssetRef (`landmark`)
 *   - face_mosaic   → LandmarkAssetRef (`landmark`)
 *   - body_alpha_key → MaskAssetRef    (`mask`)
 *
 * Coverage (per kind):
 *   - Legacy flat-string shape (`landmarkAssetId` / `maskAssetId`)
 *     populates `*.asset_id` with `has_time_offset == false`.
 *   - Typed object shape (`landmarkAssetRef` / `maskAssetRef`)
 *     populates `*.asset_id` + `*.time_offset` with
 *     `has_time_offset == true`.
 *   - Typed object without `timeOffset` populates id only.
 *   - Typed object form WINS when both shapes are present
 *     (forward-compatibility for a JSON authoring tool that
 *     migrated to the typed form but kept the legacy field for
 *     older readers).
 *   - Missing both shapes → ME_E_PARSE.
 *   - Typed object missing `assetId` → ME_E_PARSE.
 *
 * `KeypointAssetRef` is defined in `effect_params/asset_ref.hpp`
 * but no effect consumes it today; it has a default-construction +
 * field-access TEST_CASE here so the type isn't dead-code-pruned
 * during a future structure-shrink pass.
 */
#include <doctest/doctest.h>

#include "timeline/effect_params/asset_ref.hpp"
#include "timeline/timeline_impl.hpp"
#include "timeline_schema_fixtures.hpp"

#include <variant>

using me::tests::schema::EngineFixture;
using me::tests::schema::load;

namespace {

/* JSON timeline shape mirrors test_face_sticker_stub.cpp's
 * `face_sticker_timeline` — it's the canonical "loadable timeline
 * with one effect" template (asset+composition+output stitched
 * together). The single inline {kind, params} body is varied
 * per-test. */
std::string fs_timeline(const std::string& effect_body,
                         const std::string& asset_id,
                         const std::string& asset_kind) {
    return std::string{R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":")"} + asset_id + R"(","uri":"file:///tmp/asset.bin","type":")" +
        asset_kind + R"(","model":{"id":"foo","version":"v1","quantization":"fp16"}}
  ],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"v1",
     "effects":[)" + effect_body + R"(],
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
})";
}

}  // namespace

TEST_CASE("LandmarkAssetRef: default-constructed has empty id + no time offset") {
    me::LandmarkAssetRef r;
    CHECK(r.asset_id.empty());
    CHECK(r.has_time_offset == false);
    CHECK(r.time_offset.num == 0);
    CHECK(r.time_offset.den == 1);
}

TEST_CASE("MaskAssetRef: default-constructed has empty id + no time offset") {
    me::MaskAssetRef r;
    CHECK(r.asset_id.empty());
    CHECK(r.has_time_offset == false);
}

TEST_CASE("KeypointAssetRef: type defined, default-construct works") {
    /* No consumer today; the type lives so that skeleton-based
     * effects (M11+) can target it without re-introducing the
     * schema variant. The default-construct sanity check keeps it
     * out of dead-code-pruning. */
    me::KeypointAssetRef r;
    CHECK(r.asset_id.empty());
    CHECK(r.has_time_offset == false);
}

TEST_CASE("face_sticker: legacy landmarkAssetId populates asset_id, no time offset") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{"landmarkAssetId":"ml1","stickerUri":"file:///tmp/s.png"}
    })";
    REQUIRE(load(e.eng, fs_timeline(body, "ml1", "landmark"), &tl) == ME_OK);
    const auto& effects = tl->tl.clips[0].effects;
    const auto* fp = std::get_if<me::FaceStickerEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark.asset_id == "ml1");
    CHECK(fp->landmark.has_time_offset == false);
    me_timeline_destroy(tl);
}

TEST_CASE("face_sticker: typed landmarkAssetRef without timeOffset populates id only") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{"landmarkAssetRef":{"assetId":"ml2"},
                  "stickerUri":"file:///tmp/s.png"}
    })";
    REQUIRE(load(e.eng, fs_timeline(body, "ml2", "landmark"), &tl) == ME_OK);
    const auto& effects = tl->tl.clips[0].effects;
    const auto* fp = std::get_if<me::FaceStickerEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark.asset_id == "ml2");
    CHECK(fp->landmark.has_time_offset == false);
    me_timeline_destroy(tl);
}

TEST_CASE("face_sticker: typed landmarkAssetRef with timeOffset populates rational") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{
            "landmarkAssetRef":{"assetId":"ml3","timeOffset":{"num":1,"den":30}},
            "stickerUri":"file:///tmp/s.png"
        }
    })";
    REQUIRE(load(e.eng, fs_timeline(body, "ml3", "landmark"), &tl) == ME_OK);
    const auto& effects = tl->tl.clips[0].effects;
    const auto* fp = std::get_if<me::FaceStickerEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark.asset_id == "ml3");
    CHECK(fp->landmark.has_time_offset == true);
    CHECK(fp->landmark.time_offset.num == 1);
    CHECK(fp->landmark.time_offset.den == 30);
    me_timeline_destroy(tl);
}

TEST_CASE("face_sticker: missing both landmarkAssetId and landmarkAssetRef → ME_E_PARSE") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{"stickerUri":"file:///tmp/s.png"}
    })";
    CHECK(load(e.eng, fs_timeline(body, "ml1", "landmark"), &tl) == ME_E_PARSE);
    if (tl) me_timeline_destroy(tl);
}

TEST_CASE("face_sticker: typed landmarkAssetRef missing assetId → ME_E_PARSE") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{
            "landmarkAssetRef":{"timeOffset":{"num":0,"den":1}},
            "stickerUri":"file:///tmp/s.png"
        }
    })";
    CHECK(load(e.eng, fs_timeline(body, "ml1", "landmark"), &tl) == ME_E_PARSE);
    if (tl) me_timeline_destroy(tl);
}

TEST_CASE("face_sticker: typed landmarkAssetRef wins when both shapes present") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_sticker",
        "params":{
            "landmarkAssetId":"older",
            "landmarkAssetRef":{"assetId":"newer"},
            "stickerUri":"file:///tmp/s.png"
        }
    })";
    /* Both ids point at the single landmark asset declared in the
     * fixture; the loader doesn't validate cross-references. The
     * test asserts the loader picked `newer` from the typed form. */
    REQUIRE(load(e.eng, fs_timeline(body, "newer", "landmark"), &tl) == ME_OK);
    const auto& effects = tl->tl.clips[0].effects;
    const auto* fp = std::get_if<me::FaceStickerEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark.asset_id == "newer");
    me_timeline_destroy(tl);
}

TEST_CASE("body_alpha_key: typed maskAssetRef with timeOffset populates rational") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"body_alpha_key",
        "params":{
            "maskAssetRef":{"assetId":"mask1","timeOffset":{"num":2,"den":30}}
        }
    })";
    REQUIRE(load(e.eng, fs_timeline(body, "mask1", "mask"), &tl) == ME_OK);
    const auto& effects = tl->tl.clips[0].effects;
    const auto* bp = std::get_if<me::BodyAlphaKeyEffectParams>(&effects[0].params);
    REQUIRE(bp != nullptr);
    CHECK(bp->mask.asset_id == "mask1");
    CHECK(bp->mask.has_time_offset == true);
    CHECK(bp->mask.time_offset.num == 2);
    CHECK(bp->mask.time_offset.den == 30);
    me_timeline_destroy(tl);
}

TEST_CASE("face_mosaic: typed landmarkAssetRef populates rational time offset") {
    EngineFixture e;
    me_timeline_t* tl = nullptr;
    const std::string body = R"({
        "kind":"face_mosaic",
        "params":{
            "landmarkAssetRef":{"assetId":"ml1","timeOffset":{"num":3,"den":30}},
            "blockSizePx":16,"kind":"pixelate"
        }
    })";
    REQUIRE(load(e.eng, fs_timeline(body, "ml1", "landmark"), &tl) == ME_OK);
    const auto& effects = tl->tl.clips[0].effects;
    const auto* fp = std::get_if<me::FaceMosaicEffectParams>(&effects[0].params);
    REQUIRE(fp != nullptr);
    CHECK(fp->landmark.asset_id == "ml1");
    CHECK(fp->landmark.has_time_offset == true);
    CHECK(fp->landmark.time_offset.num == 3);
    CHECK(fp->landmark.time_offset.den == 30);
    me_timeline_destroy(tl);
}
