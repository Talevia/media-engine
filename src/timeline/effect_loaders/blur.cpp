/* `EffectKind::Blur` JSON loader — `radius` is required and
 * must be a number. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::BlurEffectParams parse_blur_effect_params(const json& p,
                                                const std::string& where) {
    require(p.contains("radius"), ME_E_PARSE,
            where + ": blur requires 'radius'");
    require(p["radius"].is_number(), ME_E_PARSE,
            where + ".radius: expected number");
    me::BlurEffectParams bp;
    bp.radius = p.at("radius").get<double>();
    return bp;
}

}  // namespace me::timeline_loader_detail
