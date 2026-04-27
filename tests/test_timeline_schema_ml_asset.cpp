/*
 * test_timeline_schema_ml_asset — M11 ml-asset-schema-types
 * coverage. Pins JSON parsing of the new `"type"` field on assets
 * and the required `"model"` sub-object for non-Media kinds.
 *
 * Asserted contracts:
 *   - Default (no `"type"` field) → AssetKind::Media, ml_metadata
 *     stays nullopt. Backward-compatible with all existing JSON.
 *   - `"type":"media"` is the explicit form of the default.
 *   - `"type":"landmark"|"mask"|"keypoints"` parses into the
 *     matching AssetKind enum value AND requires the `"model"`
 *     object with id / version / quantization.
 *   - Missing `"model"` on non-Media → ME_E_PARSE.
 *   - Unknown `"type"` value (typo) → ME_E_UNSUPPORTED so a
 *     typo doesn't silently degrade to Media.
 *   - `"model"` on Media kind → ME_E_PARSE (only valid when
 *     type is non-media).
 *
 * Sibling test file to test_timeline_schema_color_space.cpp.
 * Reaches into src/timeline/timeline_impl.hpp for the AssetKind
 * + MlAssetMetadata types — same src/-as-include pattern as the
 * other timeline-schema shards.
 */
#include "timeline_schema_fixtures.hpp"

#include "timeline/timeline_impl.hpp"

#include <string>

using me::tests::schema::load;
using me::tests::schema::EngineFixture;

namespace {

/* Build a timeline with a single ML asset (kind != Media) plus
 * a video asset that the clip references. The clip references the
 * video asset; the ML asset just sits in the assets map for IR
 * round-trip — not consumed by any kernel until M11 effects land. */
std::string ml_asset_timeline(const std::string& asset_type,
                               const std::string& model_id     = "blazeface_v2",
                               const std::string& model_ver    = "1.0.0",
                               const std::string& quantization = "fp16") {
    std::string j = R"({
  "schemaVersion": 1,
  "frameRate":  {"num":30,"den":1},
  "resolution": {"width":1920,"height":1080},
  "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets": [
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"ml1","uri":"file:///tmp/landmarks.bin","type":")" + asset_type + R"(",
     "model":{"id":")" + model_id + R"(","version":")" + model_ver +
     R"(","quantization":")" + quantization + R"("}}
  ],
  "compositions": [{
    "id":"main",
    "tracks":[{
      "id":"v0","kind":"video","clips":[
        {"type":"video","id":"c1","assetId":"v1",
         "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
         "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
      ]
    }]
  }],
  "output": {"compositionId":"main"}
}
)";
    return j;
}

}  // namespace

TEST_CASE("asset without type → AssetKind::Media (backward-compat)") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, me::tests::tb::minimal_video_clip().build(), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& a = tl->tl.assets.at("a1");
    CHECK(a.kind == me::AssetKind::Media);
    CHECK_FALSE(a.ml_metadata.has_value());
    me_timeline_destroy(tl);
}

TEST_CASE("asset.type=\"landmark\" parses with required model metadata") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, ml_asset_timeline("landmark"), &tl) == ME_OK);
    REQUIRE(tl != nullptr);
    const auto& a = tl->tl.assets.at("ml1");
    CHECK(a.kind == me::AssetKind::Landmark);
    REQUIRE(a.ml_metadata.has_value());
    CHECK(a.ml_metadata->model_id      == "blazeface_v2");
    CHECK(a.ml_metadata->model_version == "1.0.0");
    CHECK(a.ml_metadata->quantization  == "fp16");
    /* Sibling video asset stays Media. */
    const auto& v = tl->tl.assets.at("v1");
    CHECK(v.kind == me::AssetKind::Media);
    CHECK_FALSE(v.ml_metadata.has_value());
    me_timeline_destroy(tl);
}

TEST_CASE("asset.type=\"mask\" parses correctly") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng,
                  ml_asset_timeline("mask", "selfie_seg", "v3", "int8"),
                  &tl) == ME_OK);
    const auto& a = tl->tl.assets.at("ml1");
    CHECK(a.kind == me::AssetKind::Mask);
    REQUIRE(a.ml_metadata.has_value());
    CHECK(a.ml_metadata->model_id      == "selfie_seg");
    CHECK(a.ml_metadata->quantization  == "int8");
    me_timeline_destroy(tl);
}

TEST_CASE("asset.type=\"keypoints\" parses correctly") {
    EngineFixture f;
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng,
                  ml_asset_timeline("keypoints", "movenet", "v2", "fp32"),
                  &tl) == ME_OK);
    const auto& a = tl->tl.assets.at("ml1");
    CHECK(a.kind == me::AssetKind::Keypoints);
    REQUIRE(a.ml_metadata.has_value());
    CHECK(a.ml_metadata->model_id      == "movenet");
    me_timeline_destroy(tl);
}

TEST_CASE("asset.type=\"media\" is explicit form of the default") {
    /* Build by hand because the helper assumes ML kind. */
    EngineFixture f;
    const std::string j = R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[{"id":"a1","uri":"file:///tmp/x.mp4","type":"media"}],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"a1",
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
}
)";
    me_timeline_t* tl = nullptr;
    REQUIRE(load(f.eng, j, &tl) == ME_OK);
    const auto& a = tl->tl.assets.at("a1");
    CHECK(a.kind == me::AssetKind::Media);
    CHECK_FALSE(a.ml_metadata.has_value());
    me_timeline_destroy(tl);
}

TEST_CASE("non-media asset without model object → ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"ml1","uri":"file:///tmp/marks.bin","type":"landmark"}
  ],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"v1",
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
}
)";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
}

TEST_CASE("model fields missing one of id/version/quantization → ME_E_PARSE") {
    EngineFixture f;
    /* Missing "quantization". */
    const std::string j = R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"ml1","uri":"file:///tmp/marks.bin","type":"landmark",
     "model":{"id":"blazeface","version":"1.0"}}
  ],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"v1",
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
}
)";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
}

TEST_CASE("unknown asset.type → ME_E_UNSUPPORTED") {
    EngineFixture f;
    const std::string j = R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"v1","kind":"video","uri":"file:///tmp/vid.mp4"},
    {"id":"ml1","uri":"file:///tmp/x.bin","type":"landmrak",
     "model":{"id":"x","version":"1","quantization":"fp16"}}
  ],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"v1",
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
}
)";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_UNSUPPORTED);
}

TEST_CASE("model object on Media-kind asset → ME_E_PARSE") {
    EngineFixture f;
    const std::string j = R"({
  "schemaVersion":1,
  "frameRate":{"num":30,"den":1},
  "resolution":{"width":1920,"height":1080},
  "colorSpace":{"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
  "assets":[
    {"id":"a1","uri":"file:///tmp/x.mp4","type":"media",
     "model":{"id":"none","version":"-","quantization":"-"}}
  ],
  "compositions":[{"id":"main","tracks":[{"id":"v0","kind":"video","clips":[
    {"type":"video","id":"c1","assetId":"a1",
     "timeRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}},
     "sourceRange":{"start":{"num":0,"den":30},"duration":{"num":60,"den":30}}}
  ]}]}],
  "output":{"compositionId":"main"}
}
)";
    me_timeline_t* tl = nullptr;
    CHECK(load(f.eng, j, &tl) == ME_E_PARSE);
}
