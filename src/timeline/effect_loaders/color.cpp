/* `EffectKind::Color` JSON loader — extracted from
 * loader_helpers_clip_params.cpp's `parse_effect_spec` switch
 * (debt-split-loader-helpers-clip-params). All three params
 * (brightness / contrast / saturation) are optional doubles.
 * Defaults mean identity, so an empty params object is a valid
 * (no-op) Color effect. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::ColorEffectParams parse_color_effect_params(const json& p,
                                                 const std::string& where) {
    me::ColorEffectParams cp;
    if (p.contains("brightness")) {
        require(p["brightness"].is_number(), ME_E_PARSE,
                where + ".brightness: expected number");
        cp.brightness = p.at("brightness").get<double>();
    }
    if (p.contains("contrast")) {
        require(p["contrast"].is_number(), ME_E_PARSE,
                where + ".contrast: expected number");
        cp.contrast = p.at("contrast").get<double>();
    }
    if (p.contains("saturation")) {
        require(p["saturation"].is_number(), ME_E_PARSE,
                where + ".saturation: expected number");
        cp.saturation = p.at("saturation").get<double>();
    }
    return cp;
}

}  // namespace me::timeline_loader_detail
