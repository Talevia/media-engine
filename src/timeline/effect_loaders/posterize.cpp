/* `EffectKind::Posterize` JSON loader — M12 §156 (4/5).
 *
 * Schema:
 *   { "kind": "posterize", "params": { "levels": 4 } }
 *
 * `levels` optional (default 256 = identity). Range
 * [2, 256]. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::PosterizeEffectParams parse_posterize_effect_params(
    const json& p, const std::string& where) {
    me::PosterizeEffectParams pp;
    if (p.contains("levels")) {
        require(p["levels"].is_number_integer(), ME_E_PARSE,
                where + ".levels: expected integer");
        const std::int64_t l = p.at("levels").get<std::int64_t>();
        require(l >= 2 && l <= 256, ME_E_PARSE,
                where + ".levels: out of range (2..256)");
        pp.levels = static_cast<int>(l);
    }
    return pp;
}

}  // namespace me::timeline_loader_detail
