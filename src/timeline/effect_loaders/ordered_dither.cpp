/* `EffectKind::OrderedDither` JSON loader — M12 §156 (5/5).
 *
 * Schema:
 *   { "kind": "ordered_dither",
 *     "params": { "matrix_size": 4, "levels": 8 } }
 *
 * Both fields optional. `matrix_size` ∈ {2,4,8} (default 4).
 * `levels` ∈ [2, 256] (default 256 = effectively pure
 * dither w/o quantization). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::OrderedDitherEffectParams parse_ordered_dither_effect_params(
    const json& p, const std::string& where) {
    me::OrderedDitherEffectParams pp;
    if (p.contains("matrix_size")) {
        require(p["matrix_size"].is_number_integer(), ME_E_PARSE,
                where + ".matrix_size: expected integer");
        const std::int64_t m = p.at("matrix_size").get<std::int64_t>();
        require(m == 2 || m == 4 || m == 8, ME_E_PARSE,
                where + ".matrix_size: must be 2, 4, or 8");
        pp.matrix_size = static_cast<int>(m);
    }
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
