/* `EffectKind::ChromaticAberration` JSON loader — M12 §156
 * (3/5).
 *
 * Schema:
 *   {
 *     "kind": "chromatic_aberration",
 *     "params": {
 *       "redShift":  {"dx": 2, "dy": 0},
 *       "blueShift": {"dx": -2, "dy": 0}
 *     }
 *   }
 *
 * Both shift objects optional. Each defaults to {0, 0}. Each
 * dx/dy in [-32, 32]. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

namespace {

void parse_optional_shift(const json&        p,
                           const char*        key,
                           int*               out_dx,
                           int*               out_dy,
                           const std::string& where) {
    if (!p.contains(key)) return;
    const auto& s = p[key];
    require(s.is_object(), ME_E_PARSE,
            where + "." + key + ": expected object {dx, dy}");
    require(s.contains("dx") && s["dx"].is_number_integer(), ME_E_PARSE,
            where + "." + key + ".dx: expected integer");
    require(s.contains("dy") && s["dy"].is_number_integer(), ME_E_PARSE,
            where + "." + key + ".dy: expected integer");
    const std::int64_t dx = s.at("dx").get<std::int64_t>();
    const std::int64_t dy = s.at("dy").get<std::int64_t>();
    require(dx >= -32 && dx <= 32, ME_E_PARSE,
            where + "." + key + ".dx: out of range [-32, 32]");
    require(dy >= -32 && dy <= 32, ME_E_PARSE,
            where + "." + key + ".dy: out of range [-32, 32]");
    *out_dx = static_cast<int>(dx);
    *out_dy = static_cast<int>(dy);
}

}  // namespace

me::ChromaticAberrationEffectParams parse_chromatic_aberration_effect_params(
    const json& p, const std::string& where) {
    me::ChromaticAberrationEffectParams cap;
    parse_optional_shift(p, "redShift",  &cap.red_dx,  &cap.red_dy,  where);
    parse_optional_shift(p, "blueShift", &cap.blue_dx, &cap.blue_dy, where);
    return cap;
}

}  // namespace me::timeline_loader_detail
