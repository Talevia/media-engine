/* `EffectKind::InverseTonemap` JSON loader — registered ahead
 * of the impl per M10 exit criterion 6 + cycle 24's Linear
 * landing. The kernel returns ME_E_UNSUPPORTED for the Hable
 * algo today; loader still parses both so JSON authoring tools
 * can target either kind. `algo` ∈ {linear, hable} (default
 * linear), `targetPeakNits` > 0 (default 1000). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::InverseTonemapEffectParams parse_inverse_tonemap_effect_params(
    const json& p, const std::string& where) {
    me::InverseTonemapEffectParams ip;
    if (p.contains("algo")) {
        require(p["algo"].is_string(), ME_E_PARSE,
                where + ".algo: expected string");
        const auto algo_s = p.at("algo").get<std::string>();
        if      (algo_s == "linear") ip.algo = me::InverseTonemapEffectParams::Algo::Linear;
        else if (algo_s == "hable")  ip.algo = me::InverseTonemapEffectParams::Algo::Hable;
        else throw LoadError{ME_E_PARSE,
            where + ".algo: unknown '" + algo_s +
            "' (supported: linear, hable)"};
    }
    if (p.contains("targetPeakNits")) {
        require(p["targetPeakNits"].is_number(), ME_E_PARSE,
                where + ".targetPeakNits: expected number");
        const double n = p.at("targetPeakNits").get<double>();
        require(n > 0.0, ME_E_PARSE,
                where + ".targetPeakNits: must be > 0");
        ip.target_peak_nits = n;
    }
    return ip;
}

}  // namespace me::timeline_loader_detail
