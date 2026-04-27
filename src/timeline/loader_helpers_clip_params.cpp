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
#include "timeline/loader_helpers_keyframes.hpp"

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
    } else if (kind_str == "tonemap") {
        spec.kind = me::EffectKind::Tonemap;
        me::TonemapEffectParams tp;
        if (p.contains("algo")) {
            require(p["algo"].is_string(), ME_E_PARSE,
                    where + ".params.algo: expected string");
            const auto algo_s = p.at("algo").get<std::string>();
            if      (algo_s == "hable")    tp.algo = me::TonemapEffectParams::Algo::Hable;
            else if (algo_s == "reinhard") tp.algo = me::TonemapEffectParams::Algo::Reinhard;
            else if (algo_s == "aces")     tp.algo = me::TonemapEffectParams::Algo::ACES;
            else throw LoadError{ME_E_PARSE,
                where + ".params.algo: unknown '" + algo_s +
                "' (supported: hable, reinhard, aces)"};
        }
        if (p.contains("targetNits")) {
            require(p["targetNits"].is_number(), ME_E_PARSE,
                    where + ".params.targetNits: expected number");
            const double n = p.at("targetNits").get<double>();
            require(n > 0.0, ME_E_PARSE,
                    where + ".params.targetNits: must be > 0");
            tp.target_nits = n;
        }
        spec.params = tp;
    } else if (kind_str == "inverse_tonemap") {
        /* Reserves the API surface for SDR → HDR expansion. Loader
         * stores the typed params; the kernel
         * (compose/inverse_tonemap_kernel.cpp) returns ME_E_UNSUPPORTED
         * today — see the bullet `inverse-tonemap-effect-impl` for the
         * deferred impl. Intentionally registered now so JSON
         * authoring tools can target the kind ahead of the impl. */
        spec.kind = me::EffectKind::InverseTonemap;
        me::InverseTonemapEffectParams ip;
        if (p.contains("algo")) {
            require(p["algo"].is_string(), ME_E_PARSE,
                    where + ".params.algo: expected string");
            const auto algo_s = p.at("algo").get<std::string>();
            if      (algo_s == "linear") ip.algo = me::InverseTonemapEffectParams::Algo::Linear;
            else if (algo_s == "hable")  ip.algo = me::InverseTonemapEffectParams::Algo::Hable;
            else throw LoadError{ME_E_PARSE,
                where + ".params.algo: unknown '" + algo_s +
                "' (supported: linear, hable)"};
        }
        if (p.contains("targetPeakNits")) {
            require(p["targetPeakNits"].is_number(), ME_E_PARSE,
                    where + ".params.targetPeakNits: expected number");
            const double n = p.at("targetPeakNits").get<double>();
            require(n > 0.0, ME_E_PARSE,
                    where + ".params.targetPeakNits: must be > 0");
            ip.target_peak_nits = n;
        }
        spec.params = ip;
    } else {
        throw LoadError{ME_E_UNSUPPORTED,
                        where + ".kind: unknown effect kind '" + kind_str +
                        "' (supported: color, blur, lut, tonemap, inverse_tonemap)"};
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

    auto kfs = parse_keyframes_array<me::ColorKeyframe>(
        prop["keyframes"], where + ".keyframes",
        [](const json& v_node, const std::string& ki)
            -> std::array<std::uint8_t, 4> {
            require(v_node.is_string(), ME_E_PARSE,
                    ki + ".v: expected hex string");
            return parse_hex_rgba_strict(v_node.get<std::string>(), ki + ".v");
        });
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
