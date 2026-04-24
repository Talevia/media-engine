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
