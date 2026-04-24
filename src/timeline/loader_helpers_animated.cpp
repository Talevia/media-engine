/* Animated-number parsers — parse_transform + parse_animated_number
 * (+ anon-namespace parse_interp helper). Consumes the primitives
 * in loader_helpers_primitives.cpp and is consumed by clip-param
 * parsers + timeline_loader_tracks.cpp.
 *
 * Scope-C2 of debt-split-loader-helpers-cpp.
 */
#include "timeline/loader_helpers.hpp"
#include "timeline/loader_helpers_keyframes.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me::Transform parse_transform(const json& j, const std::string& where) {
    require(j.is_object(), ME_E_PARSE, where + ": expected object");

    /* Keys accepted by the schema (TIMELINE_SCHEMA.md §Transform). Any
     * other key is a parse error — strict rejection keeps schema
     * additions explicit and future-compatible rather than silently
     * dropping unknown fields. */
    static constexpr std::string_view known[] = {
        "translateX", "translateY",
        "scaleX", "scaleY",
        "rotationDeg", "opacity",
        "anchorX", "anchorY",
    };
    for (auto it = j.begin(); it != j.end(); ++it) {
        bool ok = false;
        for (auto k : known) { if (it.key() == k) { ok = true; break; } }
        require(ok, ME_E_PARSE,
                where + ": unknown transform key '" + it.key() + "'");
    }

    me::Transform t;  /* identity defaults per struct definition */
    auto read = [&](const char* key, me::AnimatedNumber& target) {
        if (j.contains(key)) {
            target = parse_animated_number(j[key], where + "." + key);
        }
    };
    read("translateX",  t.translate_x);
    read("translateY",  t.translate_y);
    read("scaleX",      t.scale_x);
    read("scaleY",      t.scale_y);
    read("rotationDeg", t.rotation_deg);
    read("opacity",     t.opacity);
    read("anchorX",     t.anchor_x);
    read("anchorY",     t.anchor_y);

    /* Opacity range validation: every sampled value (static or any
     * keyframe.v) must fall in [0, 1]. Other fields accept any finite
     * double (scale = 2 mirror, rotation = 720°, etc.). */
    auto validate_opacity = [&](const me::AnimatedNumber& an) {
        auto check_v = [&](double v) {
            require(v >= 0.0 && v <= 1.0, ME_E_PARSE,
                    where + ".opacity: value " + std::to_string(v) +
                    " out of [0, 1]");
        };
        if (an.static_value.has_value()) {
            check_v(*an.static_value);
        } else {
            for (const auto& kf : an.keyframes) check_v(kf.v);
        }
    };
    validate_opacity(t.opacity);

    return t;
}

me::AnimatedNumber parse_animated_number(const json& prop, const std::string& where) {
    require(prop.is_object(), ME_E_PARSE,
            where + ": expected object with \"static\" or \"keyframes\" key");

    const bool has_static = prop.contains("static");
    const bool has_kfs    = prop.contains("keyframes");
    require(has_static != has_kfs, ME_E_PARSE,
            where + ": exactly one of {\"static\", \"keyframes\"} required");

    if (has_static) {
        const auto& sv = prop["static"];
        require(sv.is_number(), ME_E_PARSE,
                where + ".static: expected number");
        return me::AnimatedNumber::from_static(sv.get<double>());
    }

    auto kfs = parse_keyframes_array<me::Keyframe>(
        prop["keyframes"], where + ".keyframes",
        [](const json& v_node, const std::string& ki) -> double {
            require(v_node.is_number(), ME_E_PARSE, ki + ".v: expected number");
            return v_node.get<double>();
        });
    return me::AnimatedNumber::from_keyframes(std::move(kfs));
}

}  // namespace me::timeline_loader_detail
