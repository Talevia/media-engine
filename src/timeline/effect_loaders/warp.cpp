/* `EffectKind::Warp` JSON loader — M12 §158 (1/2).
 *
 * Schema:
 *   { "kind": "warp",
 *     "params": {
 *       "control_points": [
 *         { "src": [0.5, 0.5], "dst": [0.5, 0.5] },
 *         ...
 *       ]
 *     } }
 *
 * `control_points` optional (default empty = identity). Max 32
 * entries. Each entry must have `src` and `dst` two-element
 * float arrays in [0, 1]. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

namespace {

void parse_xy(const json& arr, const std::string& where,
              float& out_x, float& out_y) {
    require(arr.is_array() && arr.size() == 2, ME_E_PARSE,
            where + ": expected [x, y]");
    require(arr[0].is_number() && arr[1].is_number(), ME_E_PARSE,
            where + ": expected numbers");
    const double x = arr[0].get<double>();
    const double y = arr[1].get<double>();
    require(x >= 0.0 && x <= 1.0, ME_E_PARSE, where + ".x: out of range (0..1)");
    require(y >= 0.0 && y <= 1.0, ME_E_PARSE, where + ".y: out of range (0..1)");
    out_x = static_cast<float>(x);
    out_y = static_cast<float>(y);
}

}  // namespace

me::WarpEffectParams parse_warp_effect_params(
    const json& p, const std::string& where) {
    me::WarpEffectParams pp;
    if (!p.contains("control_points")) return pp;

    const auto& cps = p.at("control_points");
    require(cps.is_array(), ME_E_PARSE,
            where + ".control_points: expected array");
    require(cps.size() <= 32, ME_E_PARSE,
            where + ".control_points: too many entries (max 32)");

    for (std::size_t i = 0; i < cps.size(); ++i) {
        const auto& cp = cps[i];
        const std::string cw = where + ".control_points[" +
                                std::to_string(i) + "]";
        require(cp.is_object(), ME_E_PARSE, cw + ": expected object");
        require(cp.contains("src") && cp.contains("dst"), ME_E_PARSE,
                cw + ": missing src/dst");
        me::WarpControlPoint wcp;
        parse_xy(cp.at("src"), cw + ".src", wcp.src_x, wcp.src_y);
        parse_xy(cp.at("dst"), cw + ".dst", wcp.dst_x, wcp.dst_y);
        pp.control_points.push_back(wcp);
    }
    return pp;
}

}  // namespace me::timeline_loader_detail
