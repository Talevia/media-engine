/* `EffectKind::MotionBlur` JSON loader — M12 §157 (1/3).
 *
 * Schema:
 *   { "kind": "motion_blur",
 *     "params": { "dx_px": 12, "dy_px": 0, "samples": 9 } }
 *
 * All fields optional. `dx_px` / `dy_px` integer pixel offsets
 * (signed, no documented hard range — kernel clamps reads at
 * image edges). `samples` ∈ [1, 64] (1 = identity). */
#include "timeline/effect_loaders/effect_loader.hpp"

#include "timeline/loader_helpers.hpp"

#include <cstdint>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::MotionBlurEffectParams parse_motion_blur_effect_params(
    const json& p, const std::string& where) {
    me::MotionBlurEffectParams pp;
    if (p.contains("dx_px")) {
        require(p["dx_px"].is_number_integer(), ME_E_PARSE,
                where + ".dx_px: expected integer");
        pp.dx_px = static_cast<int>(p.at("dx_px").get<std::int64_t>());
    }
    if (p.contains("dy_px")) {
        require(p["dy_px"].is_number_integer(), ME_E_PARSE,
                where + ".dy_px: expected integer");
        pp.dy_px = static_cast<int>(p.at("dy_px").get<std::int64_t>());
    }
    if (p.contains("samples")) {
        require(p["samples"].is_number_integer(), ME_E_PARSE,
                where + ".samples: expected integer");
        const std::int64_t s = p.at("samples").get<std::int64_t>();
        require(s >= 1 && s <= 64, ME_E_PARSE,
                where + ".samples: out of range (1..64)");
        pp.samples = static_cast<int>(s);
    }
    return pp;
}

}  // namespace me::timeline_loader_detail
