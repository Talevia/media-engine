/* `EffectKind::Vignette` JSON loader — M12 §155
 * (3/4 color effects).
 *
 * Schema:
 *   {
 *     "kind": "vignette",
 *     "params": {
 *       "radius":    0.4,
 *       "softness":  0.2,
 *       "intensity": 0.6,
 *       "centerX":   0.5,
 *       "centerY":   0.5
 *     }
 *   }
 *
 * All optional. Defaults: radius=0.5, softness=0.3,
 * intensity=0 (identity), centerX=centerY=0.5. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

namespace {

float parse_optional_float(const json&        p,
                            const char*        key,
                            float              fallback,
                            const std::string& where) {
    if (!p.contains(key)) return fallback;
    require(p[key].is_number(), ME_E_PARSE,
            where + "." + key + ": expected number");
    return p.at(key).get<float>();
}

}  // namespace

me::VignetteEffectParams parse_vignette_effect_params(
    const json& p, const std::string& where) {
    me::VignetteEffectParams vp;
    vp.radius    = parse_optional_float(p, "radius",    0.5f, where);
    vp.softness  = parse_optional_float(p, "softness",  0.3f, where);
    vp.intensity = parse_optional_float(p, "intensity", 0.0f, where);
    vp.center_x  = parse_optional_float(p, "centerX",   0.5f, where);
    vp.center_y  = parse_optional_float(p, "centerY",   0.5f, where);
    require(vp.radius   >= 0.0f, ME_E_PARSE,
            where + ".radius: must be >= 0");
    require(vp.softness >= 0.0f, ME_E_PARSE,
            where + ".softness: must be >= 0");
    return vp;
}

}  // namespace me::timeline_loader_detail
