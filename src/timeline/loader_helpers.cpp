#include "timeline/loader_helpers.hpp"

namespace me::timeline_loader_detail {

using json = nlohmann::json;

me_rational_t as_rational(const json& j, std::string_view field) {
    if (!j.is_object() || !j.contains("num") || !j.contains("den")) {
        throw LoadError{ME_E_PARSE, std::string(field) + ": expected {num,den}"};
    }
    int64_t num = j["num"].get<int64_t>();
    int64_t den = j["den"].get<int64_t>();
    if (den <= 0) throw LoadError{ME_E_PARSE, std::string(field) + ".den must be > 0"};
    return me_rational_t{num, den};
}

void require(bool cond, me_status_t s, std::string msg) {
    if (!cond) throw LoadError{s, std::move(msg)};
}

bool rational_eq(me_rational_t a, me_rational_t b) {
    /* a/b == c/d  <=>  a*d == c*b */
    return a.num * b.den == b.num * a.den;
}

/* String → enum tables for me::ColorSpace. Keep in lock-step with
 * TIMELINE_SCHEMA.md §Color and me::ColorSpace in timeline_impl.hpp.
 * Unknown string → ME_E_PARSE (caller wraps with field name). */
me::ColorSpace::Primaries to_primaries(const std::string& s) {
    using P = me::ColorSpace::Primaries;
    if (s == "bt709")  return P::BT709;
    if (s == "bt601")  return P::BT601;
    if (s == "bt2020") return P::BT2020;
    if (s == "p3-d65") return P::P3_D65;
    throw LoadError{ME_E_PARSE, "colorSpace.primaries: unknown '" + s + "'"};
}
me::ColorSpace::Transfer to_transfer(const std::string& s) {
    using T = me::ColorSpace::Transfer;
    if (s == "bt709")   return T::BT709;
    if (s == "srgb")    return T::SRGB;
    if (s == "linear")  return T::Linear;
    if (s == "pq")      return T::PQ;
    if (s == "hlg")     return T::HLG;
    if (s == "gamma22") return T::Gamma22;
    if (s == "gamma28") return T::Gamma28;
    throw LoadError{ME_E_PARSE, "colorSpace.transfer: unknown '" + s + "'"};
}
me::ColorSpace::Matrix to_matrix(const std::string& s) {
    using M = me::ColorSpace::Matrix;
    if (s == "bt709")    return M::BT709;
    if (s == "bt601")    return M::BT601;
    if (s == "bt2020nc") return M::BT2020NC;
    if (s == "identity") return M::Identity;
    throw LoadError{ME_E_PARSE, "colorSpace.matrix: unknown '" + s + "'"};
}
me::ColorSpace::Range to_range(const std::string& s) {
    using R = me::ColorSpace::Range;
    if (s == "limited") return R::Limited;
    if (s == "full")    return R::Full;
    throw LoadError{ME_E_PARSE, "colorSpace.range: unknown '" + s + "'"};
}

double parse_animated_static_number(const json& prop, const std::string& where) {
    require(prop.is_object(), ME_E_PARSE,
            where + ": expected object with {\"static\": <number>}");
    if (prop.contains("keyframes")) {
        throw LoadError{ME_E_UNSUPPORTED,
            where + ": phase-1: animated (keyframes) form not supported yet "
                    "(see transform-animated-support backlog item)"};
    }
    require(prop.contains("static"), ME_E_PARSE,
            where + ": missing \"static\" key (only static form supported in phase-1)");
    const auto& sv = prop["static"];
    require(sv.is_number(), ME_E_PARSE,
            where + ".static: expected number");
    return sv.get<double>();
}

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
            const bool known = (key == "t" || key == "v" ||
                                 key == "interp" || key == "cp");
            require(known, ME_E_PARSE,
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

me::ColorSpace parse_color_space(const json& j, const std::string& where) {
    require(j.is_object(), ME_E_PARSE, where + ".colorSpace: expected object");
    me::ColorSpace cs;
    if (j.contains("primaries")) cs.primaries = to_primaries(j["primaries"].get<std::string>());
    if (j.contains("transfer"))  cs.transfer  = to_transfer (j["transfer" ].get<std::string>());
    if (j.contains("matrix"))    cs.matrix    = to_matrix   (j["matrix"   ].get<std::string>());
    if (j.contains("range"))     cs.range     = to_range    (j["range"    ].get<std::string>());
    return cs;
}

me::EffectSpec parse_effect_spec(const json& j, const std::string& where) {
    require(j.is_object(), ME_E_PARSE, where + ": expected object");
    require(j.contains("kind"), ME_E_PARSE, where + ": missing required 'kind'");
    require(j["kind"].is_string(), ME_E_PARSE, where + ".kind: expected string");

    me::EffectSpec spec;
    const std::string kind_str = j.at("kind").get<std::string>();

    if (j.contains("id")) {
        require(j["id"].is_string(), ME_E_PARSE, where + ".id: expected string");
        spec.id = j.at("id").get<std::string>();
    }
    if (j.contains("enabled")) {
        require(j["enabled"].is_boolean(), ME_E_PARSE,
                where + ".enabled: expected bool");
        spec.enabled = j.at("enabled").get<bool>();
    }
    if (j.contains("mix")) {
        spec.mix = parse_animated_number(j["mix"], where + ".mix");
    }

    require(j.contains("params"), ME_E_PARSE,
            where + ": missing required 'params'");
    const auto& p = j["params"];
    require(p.is_object(), ME_E_PARSE, where + ".params: expected object");

    if (kind_str == "color") {
        spec.kind = me::EffectKind::Color;
        me::ColorEffectParams cp;
        if (p.contains("brightness")) {
            require(p["brightness"].is_number(), ME_E_PARSE,
                    where + ".params.brightness: expected number");
            cp.brightness = p.at("brightness").get<double>();
        }
        if (p.contains("contrast")) {
            require(p["contrast"].is_number(), ME_E_PARSE,
                    where + ".params.contrast: expected number");
            cp.contrast = p.at("contrast").get<double>();
        }
        if (p.contains("saturation")) {
            require(p["saturation"].is_number(), ME_E_PARSE,
                    where + ".params.saturation: expected number");
            cp.saturation = p.at("saturation").get<double>();
        }
        spec.params = cp;
    } else if (kind_str == "blur") {
        spec.kind = me::EffectKind::Blur;
        require(p.contains("radius"), ME_E_PARSE,
                where + ".params: blur requires 'radius'");
        require(p["radius"].is_number(), ME_E_PARSE,
                where + ".params.radius: expected number");
        me::BlurEffectParams bp;
        bp.radius = p.at("radius").get<double>();
        spec.params = bp;
    } else if (kind_str == "lut") {
        spec.kind = me::EffectKind::Lut;
        require(p.contains("lutPath"), ME_E_PARSE,
                where + ".params: lut requires 'lutPath'");
        require(p["lutPath"].is_string(), ME_E_PARSE,
                where + ".params.lutPath: expected string");
        me::LutEffectParams lp;
        lp.path = p.at("lutPath").get<std::string>();
        spec.params = lp;
    } else {
        throw LoadError{ME_E_UNSUPPORTED,
                        where + ".kind: unknown effect kind '" + kind_str +
                        "' (supported: color, blur, lut)"};
    }
    return spec;
}

namespace {

/* Validate CSS-like hex color: '#' + 6 or 8 hex digits.
 *
 * Strict parse (no named colors, no shorthand 3-digit) — matches
 * what TIMELINE_SCHEMA.md §Clip documents. Extending to shorthand
 * / named colors is a follow-up if a consumer needs it. */
bool is_valid_hex_color(const std::string& s) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        const bool hex =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

}  // namespace

me::TextClipParams parse_text_clip_params(const json& j,
                                           const std::string& where) {
    require(j.is_object(), ME_E_PARSE, where + ": expected object");

    me::TextClipParams out;

    require(j.contains("content"), ME_E_PARSE,
            where + ": missing required 'content'");
    require(j["content"].is_string(), ME_E_PARSE,
            where + ".content: expected string");
    out.content = j.at("content").get<std::string>();

    if (j.contains("color")) {
        require(j["color"].is_string(), ME_E_PARSE,
                where + ".color: expected string");
        const std::string c = j.at("color").get<std::string>();
        require(is_valid_hex_color(c), ME_E_PARSE,
                where + ".color: expected '#RRGGBB' or '#RRGGBBAA', got '" +
                c + "'");
        out.color = c;
    }

    if (j.contains("fontFamily")) {
        require(j["fontFamily"].is_string(), ME_E_PARSE,
                where + ".fontFamily: expected string");
        out.font_family = j.at("fontFamily").get<std::string>();
    }

    if (j.contains("fontSize")) {
        out.font_size = parse_animated_number(j["fontSize"],
                                                where + ".fontSize");
    }
    if (j.contains("x")) {
        out.x = parse_animated_number(j["x"], where + ".x");
    }
    if (j.contains("y")) {
        out.y = parse_animated_number(j["y"], where + ".y");
    }

    return out;
}

me::SubtitleClipParams parse_subtitle_clip_params(const json& j,
                                                    const std::string& where) {
    require(j.is_object(), ME_E_PARSE, where + ": expected object");

    me::SubtitleClipParams out;

    require(j.contains("content"), ME_E_PARSE,
            where + ": missing required 'content'");
    require(j["content"].is_string(), ME_E_PARSE,
            where + ".content: expected string");
    out.content = j.at("content").get<std::string>();

    if (j.contains("codepage")) {
        require(j["codepage"].is_string(), ME_E_PARSE,
                where + ".codepage: expected string");
        out.codepage = j.at("codepage").get<std::string>();
    }

    return out;
}

}  // namespace me::timeline_loader_detail
