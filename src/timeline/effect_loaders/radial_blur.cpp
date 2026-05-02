/* `EffectKind::RadialBlur` JSON loader — M12 §157 (2/3).
 *
 * Schema:
 *   { "kind": "radial_blur",
 *     "params": {
 *       "center_x": 0.5, "center_y": 0.5,
 *       "intensity": 0.05, "samples": 9
 *     } }
 *
 * All fields optional. center_* normalized [0,1] (default 0.5).
 * intensity ∈ [0, 1] (default 0 = identity). samples ∈ [1, 64]
 * (default 1 = identity). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::RadialBlurEffectParams parse_radial_blur_effect_params(
    const json& p, const std::string& where) {
    me::RadialBlurEffectParams pp;
    if (p.contains("center_x")) {
        require(p["center_x"].is_number(), ME_E_PARSE,
                where + ".center_x: expected number");
        const double cx = p.at("center_x").get<double>();
        require(cx >= 0.0 && cx <= 1.0, ME_E_PARSE,
                where + ".center_x: out of range (0..1)");
        pp.center_x = static_cast<float>(cx);
    }
    if (p.contains("center_y")) {
        require(p["center_y"].is_number(), ME_E_PARSE,
                where + ".center_y: expected number");
        const double cy = p.at("center_y").get<double>();
        require(cy >= 0.0 && cy <= 1.0, ME_E_PARSE,
                where + ".center_y: out of range (0..1)");
        pp.center_y = static_cast<float>(cy);
    }
    if (p.contains("intensity")) {
        require(p["intensity"].is_number(), ME_E_PARSE,
                where + ".intensity: expected number");
        const double it = p.at("intensity").get<double>();
        require(it >= 0.0 && it <= 1.0, ME_E_PARSE,
                where + ".intensity: out of range (0..1)");
        pp.intensity = static_cast<float>(it);
    }
    if (p.contains("samples")) {
        require(p["samples"].is_number_integer(), ME_E_PARSE,
                where + ".samples: expected integer");
        const std::int64_t s = p.at("samples").get<std::int64_t>();
        require(s >= 1 && s <= 64, ME_E_PARSE,
                where + ".samples: out of range (1..64)");
        pp.samples = static_cast<int>(s);
    }
    return pp;
}

}  // namespace me::timeline_loader_detail
