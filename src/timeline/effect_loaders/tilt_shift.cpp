/* `EffectKind::TiltShift` JSON loader — M12 §157 (3/3).
 *
 * Schema:
 *   { "kind": "tilt_shift",
 *     "params": {
 *       "focal_y_min": 0.4, "focal_y_max": 0.6,
 *       "edge_softness": 0.15,
 *       "max_blur_radius": 8
 *     } }
 *
 * All fields optional. focal_y_* in [0, 1] (default 0.4 / 0.6);
 * focal_y_min ≤ focal_y_max enforced. edge_softness ∈ (0, 1]
 * (default 0.2). max_blur_radius ∈ [0, 32] (default 0 =
 * identity). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::TiltShiftEffectParams parse_tilt_shift_effect_params(
    const json& p, const std::string& where) {
    me::TiltShiftEffectParams pp;
    if (p.contains("focal_y_min")) {
        require(p["focal_y_min"].is_number(), ME_E_PARSE,
                where + ".focal_y_min: expected number");
        const double v = p.at("focal_y_min").get<double>();
        require(v >= 0.0 && v <= 1.0, ME_E_PARSE,
                where + ".focal_y_min: out of range (0..1)");
        pp.focal_y_min = static_cast<float>(v);
    }
    if (p.contains("focal_y_max")) {
        require(p["focal_y_max"].is_number(), ME_E_PARSE,
                where + ".focal_y_max: expected number");
        const double v = p.at("focal_y_max").get<double>();
        require(v >= 0.0 && v <= 1.0, ME_E_PARSE,
                where + ".focal_y_max: out of range (0..1)");
        pp.focal_y_max = static_cast<float>(v);
    }
    require(pp.focal_y_min <= pp.focal_y_max, ME_E_PARSE,
            where + ": focal_y_min must be <= focal_y_max");

    if (p.contains("edge_softness")) {
        require(p["edge_softness"].is_number(), ME_E_PARSE,
                where + ".edge_softness: expected number");
        const double v = p.at("edge_softness").get<double>();
        require(v > 0.0 && v <= 1.0, ME_E_PARSE,
                where + ".edge_softness: out of range (0..1, exclusive 0)");
        pp.edge_softness = static_cast<float>(v);
    }
    if (p.contains("max_blur_radius")) {
        require(p["max_blur_radius"].is_number_integer(), ME_E_PARSE,
                where + ".max_blur_radius: expected integer");
        const std::int64_t r = p.at("max_blur_radius").get<std::int64_t>();
        require(r >= 0 && r <= 32, ME_E_PARSE,
                where + ".max_blur_radius: out of range (0..32)");
        pp.max_blur_radius = static_cast<int>(r);
    }
    return pp;
}

}  // namespace me::timeline_loader_detail
