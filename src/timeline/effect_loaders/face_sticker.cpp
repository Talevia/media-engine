/* `EffectKind::FaceSticker` JSON loader — M11 ml-effect-face-
 * sticker-stub. Cross-references an Asset.id with kind=landmark
 * (loader doesn't validate the cross-ref; compose-time consumer
 * resolves on miss). `landmarkAssetId` + `stickerUri` required;
 * `scaleX/Y` (default 1) + `offsetX/Y` (default 0) optional. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::FaceStickerEffectParams parse_face_sticker_effect_params(
    const json& p, const std::string& where) {
    me::FaceStickerEffectParams fp;
    require(p.contains("landmarkAssetId") && p["landmarkAssetId"].is_string(),
            ME_E_PARSE,
            where + ".landmarkAssetId: required string field "
            "(references an Asset.id with kind=landmark)");
    fp.landmark_asset_id = p.at("landmarkAssetId").get<std::string>();
    require(p.contains("stickerUri") && p["stickerUri"].is_string(),
            ME_E_PARSE,
            where + ".stickerUri: required string field");
    fp.sticker_uri = p.at("stickerUri").get<std::string>();
    if (p.contains("scaleX")) {
        require(p["scaleX"].is_number(), ME_E_PARSE,
                where + ".scaleX: expected number");
        fp.scale_x = p.at("scaleX").get<double>();
    }
    if (p.contains("scaleY")) {
        require(p["scaleY"].is_number(), ME_E_PARSE,
                where + ".scaleY: expected number");
        fp.scale_y = p.at("scaleY").get<double>();
    }
    if (p.contains("offsetX")) {
        require(p["offsetX"].is_number(), ME_E_PARSE,
                where + ".offsetX: expected number");
        fp.offset_x = p.at("offsetX").get<double>();
    }
    if (p.contains("offsetY")) {
        require(p["offsetY"].is_number(), ME_E_PARSE,
                where + ".offsetY: expected number");
        fp.offset_y = p.at("offsetY").get<double>();
    }
    return fp;
}

}  // namespace me::timeline_loader_detail
