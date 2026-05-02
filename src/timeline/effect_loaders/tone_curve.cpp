/* `EffectKind::ToneCurve` JSON loader — M12 §155 (1/4 color
 * effects).
 *
 * Schema:
 *   {
 *     "kind": "tone_curve",
 *     "params": {
 *       "r": [{"x": 0, "y": 0}, {"x": 128, "y": 96}, {"x": 255, "y": 255}],
 *       "g": [],
 *       "b": [{"x": 0, "y": 0}, {"x": 255, "y": 255}]
 *     }
 *   }
 *
 * All three channel arrays optional (empty array OR field
 * omitted → identity for that channel). When non-empty, the
 * array must contain ≥ 2 points; each point's `x` and `y` are
 * uint8 (0..255); points must be sorted by `x`. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

namespace {

std::vector<me::ToneCurvePoint> parse_curve(const json&        arr,
                                              const std::string& where) {
    std::vector<me::ToneCurvePoint> out;
    if (arr.empty()) return out;
    require(arr.size() >= 2, ME_E_PARSE,
            where + ": curve must have at least 2 points (got " +
            std::to_string(arr.size()) + ")");
    out.reserve(arr.size());
    int prev_x = -1;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& pt = arr[i];
        const std::string pt_where = where + "[" + std::to_string(i) + "]";
        require(pt.is_object() && pt.contains("x") && pt.contains("y") &&
                  pt["x"].is_number_integer() && pt["y"].is_number_integer(),
                ME_E_PARSE,
                pt_where + ": expected {x: int, y: int} object");
        const int xv = pt.at("x").get<int>();
        const int yv = pt.at("y").get<int>();
        require(xv >= 0 && xv <= 255, ME_E_PARSE,
                pt_where + ".x: must be in [0, 255] (got " +
                std::to_string(xv) + ")");
        require(yv >= 0 && yv <= 255, ME_E_PARSE,
                pt_where + ".y: must be in [0, 255] (got " +
                std::to_string(yv) + ")");
        require(xv > prev_x, ME_E_PARSE,
                pt_where + ".x: points must be strictly sorted by x (got " +
                std::to_string(xv) + " after " + std::to_string(prev_x) + ")");
        prev_x = xv;
        out.push_back({static_cast<std::uint8_t>(xv),
                        static_cast<std::uint8_t>(yv)});
    }
    return out;
}

void parse_optional_curve(const json&                       p,
                           const char*                       key,
                           std::vector<me::ToneCurvePoint>* out,
                           const std::string&                where) {
    if (!p.contains(key)) return;
    require(p[key].is_array(), ME_E_PARSE,
            where + "." + key + ": expected array");
    *out = parse_curve(p.at(key), where + "." + key);
}

}  // namespace

me::ToneCurveEffectParams parse_tone_curve_effect_params(
    const json& p, const std::string& where) {
    me::ToneCurveEffectParams tcp;
    parse_optional_curve(p, "r", &tcp.r, where);
    parse_optional_curve(p, "g", &tcp.g, where);
    parse_optional_curve(p, "b", &tcp.b, where);
    return tcp;
}

}  // namespace me::timeline_loader_detail
