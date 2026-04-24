/* Animated-number parsers — parse_transform + parse_animated_number
 * (+ anon-namespace parse_interp helper). Consumes the primitives
 * in loader_helpers_primitives.cpp and is consumed by clip-param
 * parsers + timeline_loader_tracks.cpp.
 *
 * Scope-C2 of debt-split-loader-helpers-cpp.
 */
#include "timeline/loader_helpers.hpp"

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

namespace {

me::Interp parse_interp(const json& v, const std::string& where) {
    require(v.is_string(), ME_E_PARSE, where + ".interp: expected string");
    const std::string s = v.get<std::string>();
    if (s == "linear")  return me::Interp::Linear;
    if (s == "bezier")  return me::Interp::Bezier;
    if (s == "hold")    return me::Interp::Hold;
    if (s == "stepped") return me::Interp::Stepped;
    throw LoadError{ME_E_PARSE,
        where + ".interp: unknown '" + s + "' (expected linear/bezier/hold/stepped)"};
}

}  // namespace

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

    /* Keyframed form. */
    const auto& arr = prop["keyframes"];
    require(arr.is_array(), ME_E_PARSE,
            where + ".keyframes: expected array");
    require(!arr.empty(), ME_E_PARSE,
            where + ".keyframes: at least one keyframe required");

    std::vector<me::Keyframe> kfs;
    kfs.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& k = arr[i];
        const std::string ki = where + ".keyframes[" + std::to_string(i) + "]";
        require(k.is_object(), ME_E_PARSE, ki + ": expected object");
        require(k.contains("t"), ME_E_PARSE, ki + ".t: missing");
        require(k.contains("v"), ME_E_PARSE, ki + ".v: missing");
        require(k.contains("interp"), ME_E_PARSE, ki + ".interp: missing");

        me::Keyframe kf;
        kf.t      = as_rational(k["t"], ki + ".t");
        require(k["v"].is_number(), ME_E_PARSE, ki + ".v: expected number");
        kf.v      = k["v"].get<double>();
        kf.interp = parse_interp(k["interp"], ki);

        if (kf.interp == me::Interp::Bezier) {
            require(k.contains("cp"), ME_E_PARSE,
                    ki + ".cp: required when interp=bezier");
            const auto& cp = k["cp"];
            require(cp.is_array() && cp.size() == 4, ME_E_PARSE,
                    ki + ".cp: expected 4-element array");
            for (int j = 0; j < 4; ++j) {
                require(cp[j].is_number(), ME_E_PARSE,
                        ki + ".cp[" + std::to_string(j) + "]: expected number");
                kf.cp[j] = cp[j].get<double>();
            }
            /* x1 / x2 (indices 0 and 2) must be in [0, 1] per CSS
             * cubic-bezier constraints — out-of-range produces
             * non-monotonic x(s), breaks Newton iteration. */
            require(kf.cp[0] >= 0.0 && kf.cp[0] <= 1.0, ME_E_PARSE,
                    ki + ".cp[0] (x1): must be in [0, 1]");
            require(kf.cp[2] >= 0.0 && kf.cp[2] <= 1.0, ME_E_PARSE,
                    ki + ".cp[2] (x2): must be in [0, 1]");
        }
        /* No extra keys allowed beyond {t, v, interp, cp}. */
        for (auto it = k.begin(); it != k.end(); ++it) {
            const std::string& key = it.key();
            const bool is_known = (key == "t" || key == "v" ||
                                    key == "interp" || key == "cp");
            require(is_known, ME_E_PARSE,
                    ki + ": unknown keyframe key '" + key + "'");
        }

        kfs.push_back(kf);
    }

    /* Sorted-by-t + no-dup validation (schema invariant). */
    for (std::size_t i = 1; i < kfs.size(); ++i) {
        const me_rational_t prev_t = kfs[i - 1].t;
        const me_rational_t this_t = kfs[i].t;
        /* prev.t < this.t strictly (no dup, no inversion). */
        const int64_t lhs = prev_t.num * this_t.den;
        const int64_t rhs = this_t.num * prev_t.den;
        require(lhs < rhs, ME_E_PARSE,
                where + ".keyframes: must be strictly sorted by t (no duplicates, no inversion); "
                "issue at index " + std::to_string(i));
    }

    return me::AnimatedNumber::from_keyframes(std::move(kfs));
}

}  // namespace me::timeline_loader_detail
