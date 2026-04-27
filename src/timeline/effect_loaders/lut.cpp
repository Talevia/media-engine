/* `EffectKind::Lut` JSON loader — `lutPath` is required and
 * must be a string. Asset_ref resolution (file://, etc.) is
 * deferred to the kernel. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::LutEffectParams parse_lut_effect_params(const json& p,
                                              const std::string& where) {
    require(p.contains("lutPath"), ME_E_PARSE,
            where + ": lut requires 'lutPath'");
    require(p["lutPath"].is_string(), ME_E_PARSE,
            where + ".lutPath: expected string");
    me::LutEffectParams lp;
    lp.path = p.at("lutPath").get<std::string>();
    return lp;
}

}  // namespace me::timeline_loader_detail
