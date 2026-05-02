/* `EffectKind::FilmGrain` JSON loader — M12 §155
 * (4/4 color effects).
 *
 * Schema:
 *   {
 *     "kind": "film_grain",
 *     "params": {
 *       "seed":         12345,
 *       "amount":       0.1,
 *       "grainSizePx":  2
 *     }
 *   }
 *
 * `seed` accepts integer (parsed as int64; uint64 cast in
 * the kernel). All optional. Defaults: seed=0, amount=0
 * (identity), grainSizePx=1. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::FilmGrainEffectParams parse_film_grain_effect_params(
    const json& p, const std::string& where) {
    me::FilmGrainEffectParams fgp;

    if (p.contains("seed")) {
        require(p["seed"].is_number_integer(), ME_E_PARSE,
                where + ".seed: expected integer");
        /* Accept negative int64 — wraps to large uint64 via
         * two's complement, which is fine for the PRNG seed. */
        const std::int64_t s = p.at("seed").get<std::int64_t>();
        fgp.seed = static_cast<std::uint64_t>(s);
    }

    if (p.contains("amount")) {
        require(p["amount"].is_number(), ME_E_PARSE,
                where + ".amount: expected number");
        fgp.amount = p.at("amount").get<float>();
    }

    if (p.contains("grainSizePx")) {
        require(p["grainSizePx"].is_number_integer(), ME_E_PARSE,
                where + ".grainSizePx: expected integer");
        const std::int64_t gs = p.at("grainSizePx").get<std::int64_t>();
        require(gs >= 1 && gs <= 8, ME_E_PARSE,
                where + ".grainSizePx: out of range (1..8)");
        fgp.grain_size_px = static_cast<int>(gs);
    }
    return fgp;
}

}  // namespace me::timeline_loader_detail
