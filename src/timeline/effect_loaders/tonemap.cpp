/* `EffectKind::Tonemap` JSON loader — `algo` ∈
 * {hable, reinhard, aces} (default hable), `targetNits` > 0
 * (default 100). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::TonemapEffectParams parse_tonemap_effect_params(const json& p,
                                                     const std::string& where) {
    me::TonemapEffectParams tp;
    if (p.contains("algo")) {
        require(p["algo"].is_string(), ME_E_PARSE,
                where + ".algo: expected string");
        const auto algo_s = p.at("algo").get<std::string>();
        if      (algo_s == "hable")    tp.algo = me::TonemapEffectParams::Algo::Hable;
        else if (algo_s == "reinhard") tp.algo = me::TonemapEffectParams::Algo::Reinhard;
        else if (algo_s == "aces")     tp.algo = me::TonemapEffectParams::Algo::ACES;
        else throw LoadError{ME_E_PARSE,
            where + ".algo: unknown '" + algo_s +
            "' (supported: hable, reinhard, aces)"};
    }
    if (p.contains("targetNits")) {
        require(p["targetNits"].is_number(), ME_E_PARSE,
                where + ".targetNits: expected number");
        const double n = p.at("targetNits").get<double>();
        require(n > 0.0, ME_E_PARSE,
                where + ".targetNits: must be > 0");
        tp.target_nits = n;
    }
    return tp;
}

}  // namespace me::timeline_loader_detail
