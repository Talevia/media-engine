/* Clip-level parameter parsers — parse_effect_spec +
 * parse_text_clip_params + parse_subtitle_clip_params (+ anon
 * is_valid_hex_color helper for the text color field).
 *
 * Scope-C3 of debt-split-loader-helpers-cpp. Top of the helper
 * dependency tree: consumes parse_animated_number
 * (loader_helpers_animated.cpp) + the primitive require()
 * (loader_helpers_primitives.cpp).
 */
#include "timeline/loader_helpers.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

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

std::uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    return static_cast<std::uint8_t>(c - 'A' + 10);
}
std::uint8_t hex_byte(char hi, char lo) {
    return static_cast<std::uint8_t>((hex_nibble(hi) << 4) | hex_nibble(lo));
}

std::array<std::uint8_t, 4> parse_hex_rgba_strict(const std::string& s,
                                                    const std::string& where) {
    require(is_valid_hex_color(s), ME_E_PARSE,
            where + ": expected '#RRGGBB' or '#RRGGBBAA', got '" + s + "'");
    std::array<std::uint8_t, 4> out{0xFF, 0xFF, 0xFF, 0xFF};
    out[0] = hex_byte(s[1], s[2]);
    out[1] = hex_byte(s[3], s[4]);
    out[2] = hex_byte(s[5], s[6]);
    if (s.size() == 9) {
        out[3] = hex_byte(s[7], s[8]);
    }
    return out;
}

me::Interp parse_interp_str(const std::string& s, const std::string& where) {
    if (s == "linear")  return me::Interp::Linear;
    if (s == "bezier")  return me::Interp::Bezier;
    if (s == "hold")    return me::Interp::Hold;
    if (s == "stepped") return me::Interp::Stepped;
    throw LoadError{ME_E_PARSE,
        where + ".interp: unknown '" + s + "' (expected linear/bezier/hold/stepped)"};
}

}  // namespace

me::AnimatedColor parse_animated_color(const json& prop, const std::string& where) {
    /* Shape A: plain hex string ("legacy" color field). */
    if (prop.is_string()) {
        const std::string s = prop.get<std::string>();
        return me::AnimatedColor::from_static(parse_hex_rgba_strict(s, where));
    }

    require(prop.is_object(), ME_E_PARSE,
            where + ": expected hex string or {static|keyframes} object");

    const bool has_static = prop.contains("static");
    const bool has_kfs    = prop.contains("keyframes");
    require(has_static != has_kfs, ME_E_PARSE,
            where + ": exactly one of {\"static\", \"keyframes\"} required");

    if (has_static) {
        const auto& sv = prop["static"];
        require(sv.is_string(), ME_E_PARSE,
                where + ".static: expected hex string");
        return me::AnimatedColor::from_static(
            parse_hex_rgba_strict(sv.get<std::string>(), where + ".static"));
    }

    /* Keyframed form — mirrors parse_animated_number's keyframe walker
     * but with hex-string `v`. Adjacent-pair sort-by-t invariant is
     * the same. */
    const auto& arr = prop["keyframes"];
    require(arr.is_array(), ME_E_PARSE, where + ".keyframes: expected array");
    require(!arr.empty(), ME_E_PARSE,
            where + ".keyframes: at least one keyframe required");

    std::vector<me::ColorKeyframe> kfs;
    kfs.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& k  = arr[i];
        const std::string ki = where + ".keyframes[" + std::to_string(i) + "]";
        require(k.is_object(), ME_E_PARSE, ki + ": expected object");
        require(k.contains("t"), ME_E_PARSE, ki + ".t: missing");
        require(k.contains("v"), ME_E_PARSE, ki + ".v: missing");
        require(k.contains("interp"), ME_E_PARSE, ki + ".interp: missing");

        me::ColorKeyframe kf;
        kf.t      = as_rational(k["t"], ki + ".t");
        require(k["v"].is_string(), ME_E_PARSE, ki + ".v: expected hex string");
        kf.v      = parse_hex_rgba_strict(k["v"].get<std::string>(), ki + ".v");
        require(k["interp"].is_string(), ME_E_PARSE, ki + ".interp: expected string");
        kf.interp = parse_interp_str(k["interp"].get<std::string>(), ki);

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

    /* Strict sort-by-t (no dup / no inversion). */
    for (std::size_t i = 1; i < kfs.size(); ++i) {
        const me_rational_t pt = kfs[i - 1].t;
        const me_rational_t tt = kfs[i].t;
        const int64_t lhs = pt.num * tt.den;
        const int64_t rhs = tt.num * pt.den;
        require(lhs < rhs, ME_E_PARSE,
                where + ".keyframes: must be strictly sorted by t (no duplicates, no inversion); "
                "issue at index " + std::to_string(i));
    }

    return me::AnimatedColor::from_keyframes(std::move(kfs));
}

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
        out.color = parse_animated_color(j["color"], where + ".color");
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

    if (j.contains("maxWidth")) {
        require(j["maxWidth"].is_number(), ME_E_PARSE,
                where + ".maxWidth: expected number");
        const double w = j.at("maxWidth").get<double>();
        require(w > 0.0, ME_E_PARSE,
                where + ".maxWidth: must be positive");
        out.max_width = w;
    }
    if (j.contains("lineHeightMultiplier")) {
        require(j["lineHeightMultiplier"].is_number(), ME_E_PARSE,
                where + ".lineHeightMultiplier: expected number");
        const double m = j.at("lineHeightMultiplier").get<double>();
        require(m > 0.0, ME_E_PARSE,
                where + ".lineHeightMultiplier: must be positive");
        out.line_height_multiplier = m;
    }

    return out;
}

me::SubtitleClipParams parse_subtitle_clip_params(const json& j,
                                                    const std::string& where) {
    require(j.is_object(), ME_E_PARSE, where + ": expected object");

    me::SubtitleClipParams out;

    const bool has_content  = j.contains("content");
    const bool has_file_uri = j.contains("fileUri");
    require(has_content || has_file_uri, ME_E_PARSE,
            where + ": missing required 'content' or 'fileUri'");
    require(!(has_content && has_file_uri), ME_E_PARSE,
            where + ": exactly one of {'content', 'fileUri'} may be set");

    if (has_content) {
        require(j["content"].is_string(), ME_E_PARSE,
                where + ".content: expected string");
        out.content = j.at("content").get<std::string>();
    } else {
        require(j["fileUri"].is_string(), ME_E_PARSE,
                where + ".fileUri: expected string");
        out.file_uri = j.at("fileUri").get<std::string>();
        require(!out.file_uri.empty(), ME_E_PARSE,
                where + ".fileUri: must be non-empty");
    }

    if (j.contains("codepage")) {
        require(j["codepage"].is_string(), ME_E_PARSE,
                where + ".codepage: expected string");
        out.codepage = j.at("codepage").get<std::string>();
    }

    return out;
}

}  // namespace me::timeline_loader_detail
