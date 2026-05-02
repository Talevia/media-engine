/* `EffectKind::ScanLines` JSON loader — M12 §156 (2/5). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::ScanLinesEffectParams parse_scan_lines_effect_params(
    const json& p, const std::string& where) {
    me::ScanLinesEffectParams sp;
    if (p.contains("lineHeightPx")) {
        require(p["lineHeightPx"].is_number_integer(), ME_E_PARSE,
                where + ".lineHeightPx: expected integer");
        const std::int64_t lh = p.at("lineHeightPx").get<std::int64_t>();
        require(lh >= 1 && lh <= 64, ME_E_PARSE,
                where + ".lineHeightPx: out of range (1..64)");
        sp.line_height_px = static_cast<int>(lh);
    }
    if (p.contains("darkness")) {
        require(p["darkness"].is_number(), ME_E_PARSE,
                where + ".darkness: expected number");
        sp.darkness = p.at("darkness").get<float>();
    }
    if (p.contains("phaseOffsetPx")) {
        require(p["phaseOffsetPx"].is_number_integer(), ME_E_PARSE,
                where + ".phaseOffsetPx: expected integer");
        const std::int64_t po = p.at("phaseOffsetPx").get<std::int64_t>();
        require(po >= 0 && po < 64, ME_E_PARSE,
                where + ".phaseOffsetPx: out of range (0..63)");
        sp.phase_offset_px = static_cast<int>(po);
    }
    return sp;
}

}  // namespace me::timeline_loader_detail
