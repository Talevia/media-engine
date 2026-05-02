/* `EffectKind::FaceMosaic` JSON loader — M11 ml-effect-face-
 * mosaic-stub. Schema (cycle 31
 * `effect-kind-ml-asset-input-schema`): either `landmarkAssetId`
 * (legacy flat string) or `landmarkAssetRef` (typed object) —
 * exactly one required. `blockSizePx` ∈ (0, 1024] (default 16);
 * `kind` ∈ {pixelate, blur} (default pixelate). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::FaceMosaicEffectParams parse_face_mosaic_effect_params(
    const json& p, const std::string& where) {
    me::FaceMosaicEffectParams fmp;
    if (p.contains("landmarkAssetRef") && p["landmarkAssetRef"].is_object()) {
        parse_asset_ref_object(p.at("landmarkAssetRef"),
                                where + ".landmarkAssetRef",
                                fmp.landmark.asset_id,
                                fmp.landmark.time_offset,
                                fmp.landmark.has_time_offset);
    } else {
        require(p.contains("landmarkAssetId") && p["landmarkAssetId"].is_string(),
                ME_E_PARSE,
                where + ".landmarkAssetId: required string field "
                "(or .landmarkAssetRef object with assetId)");
        fmp.landmark.asset_id = p.at("landmarkAssetId").get<std::string>();
    }
    if (p.contains("blockSizePx")) {
        require(p["blockSizePx"].is_number_integer(), ME_E_PARSE,
                where + ".blockSizePx: expected integer");
        const int64_t b = p.at("blockSizePx").get<int64_t>();
        require(b > 0 && b <= 1024, ME_E_PARSE,
                where + ".blockSizePx: out of range (1..1024)");
        fmp.block_size_px = static_cast<int>(b);
    }
    if (p.contains("kind")) {
        require(p["kind"].is_string(), ME_E_PARSE,
                where + ".kind: expected string");
        const auto kk = p.at("kind").get<std::string>();
        if      (kk == "pixelate") fmp.kind = me::FaceMosaicEffectParams::Kind::Pixelate;
        else if (kk == "blur")     fmp.kind = me::FaceMosaicEffectParams::Kind::Blur;
        else throw LoadError{ME_E_PARSE,
            where + ".kind: unknown '" + kk +
            "' (supported: pixelate, blur)"};
    }
    return fmp;
}

}  // namespace me::timeline_loader_detail
