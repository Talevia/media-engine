/* `EffectKind::Glitch` JSON loader — M12 §156
 * (1/5 stylized effects).
 *
 * Schema:
 *   {
 *     "kind": "glitch",
 *     "params": {
 *       "seed":              42,
 *       "intensity":         0.1,
 *       "blockSizePx":       8,
 *       "channelShiftMaxPx": 4
 *     }
 *   }
 *
 * All optional. Defaults: seed=0, intensity=0 (identity),
 * blockSizePx=8, channelShiftMaxPx=0. */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::GlitchEffectParams parse_glitch_effect_params(
    const json& p, const std::string& where) {
    me::GlitchEffectParams gp;

    if (p.contains("seed")) {
        require(p["seed"].is_number_integer(), ME_E_PARSE,
                where + ".seed: expected integer");
        const std::int64_t s = p.at("seed").get<std::int64_t>();
        gp.seed = static_cast<std::uint64_t>(s);
    }
    if (p.contains("intensity")) {
        require(p["intensity"].is_number(), ME_E_PARSE,
                where + ".intensity: expected number");
        gp.intensity = p.at("intensity").get<float>();
    }
    if (p.contains("blockSizePx")) {
        require(p["blockSizePx"].is_number_integer(), ME_E_PARSE,
                where + ".blockSizePx: expected integer");
        const std::int64_t bs = p.at("blockSizePx").get<std::int64_t>();
        require(bs >= 1 && bs <= 64, ME_E_PARSE,
                where + ".blockSizePx: out of range (1..64)");
        gp.block_size_px = static_cast<int>(bs);
    }
    if (p.contains("channelShiftMaxPx")) {
        require(p["channelShiftMaxPx"].is_number_integer(), ME_E_PARSE,
                where + ".channelShiftMaxPx: expected integer");
        const std::int64_t cs = p.at("channelShiftMaxPx").get<std::int64_t>();
        require(cs >= 0 && cs <= 16, ME_E_PARSE,
                where + ".channelShiftMaxPx: out of range (0..16)");
        gp.channel_shift_max_px = static_cast<int>(cs);
    }
    return gp;
}

}  // namespace me::timeline_loader_detail
