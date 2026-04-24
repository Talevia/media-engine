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
    auto read = [&](const char* key, double& target) {
        if (j.contains(key)) {
            target = parse_animated_static_number(j[key], where + "." + key);
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

    /* Opacity is the only field with an intrinsic range constraint —
     * out-of-range values would produce undefined alpha blending. Other
     * fields (scale/rotation/translate) accept any finite double including
     * negatives (mirror, counter-rotate). */
    require(t.opacity >= 0.0 && t.opacity <= 1.0, ME_E_PARSE,
            where + ".opacity: must be in [0, 1]");

    return t;
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

}  // namespace me::timeline_loader_detail
