/* `EffectKind::Displacement` JSON loader — M12 §158 (2/2).
 *
 * Schema:
 *   { "kind": "displacement",
 *     "params": {
 *       "texture_uri": "file:///path/to/displacement.png",
 *       "strength_x": 8.0,
 *       "strength_y": 4.0
 *     } }
 *
 * `texture_uri` is required and must be non-empty if either
 * strength is non-zero — otherwise the kernel will reject. We
 * allow empty here so a fully-default-constructed param (= no-
 * op) parses cleanly. strength_x/y range [-256, 256] (sane
 * upper bound to catch typos; far beyond useful displacement). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::DisplacementEffectParams parse_displacement_effect_params(
    const json& p, const std::string& where) {
    me::DisplacementEffectParams pp;
    if (p.contains("texture_uri")) {
        require(p["texture_uri"].is_string(), ME_E_PARSE,
                where + ".texture_uri: expected string");
        pp.texture_uri = p.at("texture_uri").get<std::string>();
    }
    if (p.contains("strength_x")) {
        require(p["strength_x"].is_number(), ME_E_PARSE,
                where + ".strength_x: expected number");
        const double v = p.at("strength_x").get<double>();
        require(v >= -256.0 && v <= 256.0, ME_E_PARSE,
                where + ".strength_x: out of range (-256..256)");
        pp.strength_x = static_cast<float>(v);
    }
    if (p.contains("strength_y")) {
        require(p["strength_y"].is_number(), ME_E_PARSE,
                where + ".strength_y: expected number");
        const double v = p.at("strength_y").get<double>();
        require(v >= -256.0 && v <= 256.0, ME_E_PARSE,
                where + ".strength_y: out of range (-256..256)");
        pp.strength_y = static_cast<float>(v);
    }
    return pp;
}

}  // namespace me::timeline_loader_detail
