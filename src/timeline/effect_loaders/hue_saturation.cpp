/* `EffectKind::HueSaturation` JSON loader — M12 §155
 * (2/4 color effects).
 *
 * Schema:
 *   {
 *     "kind": "hue_saturation",
 *     "params": {
 *       "hueShiftDeg":    -30.0,
 *       "saturationScale":  1.5,
 *       "lightnessScale":   0.9
 *     }
 *   }
 *
 * All three optional. Defaults: hueShiftDeg=0,
 * saturationScale=1, lightnessScale=1 (identity transform).
 * Values out of conventional range (e.g. hueShiftDeg outside
 * [-180, 180]) are accepted; the kernel wraps hue mod 360°
 * + clamps S/L on the high side. Negative scales clamp to 0
 * (kernel-side); the loader doesn't reject them so a user
 * can express "fully desaturate" via saturationScale=0. */
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

me::HueSaturationEffectParams parse_hue_saturation_effect_params(
    const json& p, const std::string& where) {
    me::HueSaturationEffectParams hsp;
    hsp.hue_shift_deg    = parse_optional_float(p, "hueShiftDeg",    0.0f, where);
    hsp.saturation_scale = parse_optional_float(p, "saturationScale", 1.0f, where);
    hsp.lightness_scale  = parse_optional_float(p, "lightnessScale",  1.0f, where);
    return hsp;
}

}  // namespace me::timeline_loader_detail
