/* `EffectKind::BodyAlphaKey` JSON loader — M11 ml-effect-body-
 * alpha-key-stub. `maskAssetId` required; `featherRadiusPx` ∈
 * [0, 256] (default 0); `invert` boolean (default false). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::BodyAlphaKeyEffectParams parse_body_alpha_key_effect_params(
    const json& p, const std::string& where) {
    me::BodyAlphaKeyEffectParams bp;
    require(p.contains("maskAssetId") && p["maskAssetId"].is_string(),
            ME_E_PARSE,
            where + ".maskAssetId: required string field "
            "(references an Asset.id with kind=mask)");
    bp.mask_asset_id = p.at("maskAssetId").get<std::string>();
    if (p.contains("featherRadiusPx")) {
        require(p["featherRadiusPx"].is_number_integer(), ME_E_PARSE,
                where + ".featherRadiusPx: expected integer");
        const int64_t r = p.at("featherRadiusPx").get<int64_t>();
        require(r >= 0 && r <= 256, ME_E_PARSE,
                where + ".featherRadiusPx: out of range (0..256)");
        bp.feather_radius_px = static_cast<int>(r);
    }
    if (p.contains("invert")) {
        require(p["invert"].is_boolean(), ME_E_PARSE,
                where + ".invert: expected boolean");
        bp.invert = p.at("invert").get<bool>();
    }
    return bp;
}

}  // namespace me::timeline_loader_detail
