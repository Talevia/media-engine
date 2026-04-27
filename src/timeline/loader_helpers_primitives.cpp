/* Primitive parse helpers — rational / require / rational_eq +
 * ColorSpace enum-string shims + static-number gate +
 * parse_color_space. Consumed by every other loader helper TU.
 *
 * Scope-C1 of debt-split-loader-helpers-cpp. The pre-split
 * loader_helpers.cpp was 401 lines; these primitives are the bottom
 * of the dependency tree (no calls into animated / clip-param
 * parsers), so they land in their own file to form a stable base.
 */
#include "timeline/loader_helpers.hpp"

#include <string>
#include <string_view>
#include <utility>

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
 * TIMELINE_SCHEMA.md §Color and me::ColorSpace in
 * timeline_ir_params.hpp. Unknown string → ME_E_PARSE. */
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
    if (s == "bt709")        return T::BT709;
    if (s == "srgb")         return T::SRGB;
    if (s == "linear")       return T::Linear;
    if (s == "pq")           return T::PQ;
    /* `smpte2084` is the formal SMPTE name PQ; libavutil reports
     * exactly this string for HDR10 sources, and `me_probe` returns
     * it verbatim (probe.h:53). Accepting the alias lets host
     * tooling round-trip probe output → timeline JSON without
     * post-processing the string. */
    if (s == "smpte2084")    return T::PQ;
    if (s == "hlg")          return T::HLG;
    /* `arib-std-b67` is the formal ARIB name for HLG (BBC/NHK
     * Hybrid Log-Gamma per ARIB STD-B67). Same round-trip rationale
     * as `smpte2084` above. */
    if (s == "arib-std-b67") return T::HLG;
    if (s == "gamma22")      return T::Gamma22;
    if (s == "gamma28")      return T::Gamma28;
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

/* Cross-axis ColorSpace consistency. Each axis parses independently
 * above (`Unspecified` is always allowed), but some specified-axis
 * combinations describe video that no real display can encode/decode
 * — silently accepting them makes M10 HDR dispatch inherit garbage
 * inputs. Rules below are the ones the industry actually enforces;
 * we deliberately do NOT police every nuance (e.g. BT.2020 primaries
 * with BT.709 transfer for narrow-gamut SDR is legitimate and stays
 * accepted). `Unspecified` on either side of a rule means "host
 * trusts container metadata", which we can't second-guess at parse
 * time, so the rule short-circuits.
 *
 *  R1. PQ / HLG transfer ⇒ primaries must be BT.2020. PQ (HDR10) and
 *      HLG (BT.2100) are both defined only over BT.2020 primaries;
 *      pairing them with BT.709 / BT.601 / P3-D65 is a mis-tagging
 *      bug, not a usable color space.
 *  R2. BT.2020NC matrix ⇒ primaries must be BT.2020. The non-constant
 *      luminance Y'CbCr coefficients in this matrix are derived from
 *      the BT.2020 primaries; using them with BT.709 primaries
 *      produces wrong luma.
 *  R3. Identity matrix ⇒ range must be Full. Identity matrix means
 *      the YCbCr planes are actually GBR (no chroma subsampling
 *      math); the H.264/HEVC spec mandates Full range for that case.
 */
void validate_color_space_combo(const me::ColorSpace& cs, const std::string& where) {
    using P = me::ColorSpace::Primaries;
    using T = me::ColorSpace::Transfer;
    using M = me::ColorSpace::Matrix;
    using R = me::ColorSpace::Range;

    /* R1: HDR transfers require BT.2020 primaries. */
    if ((cs.transfer == T::PQ || cs.transfer == T::HLG) &&
        cs.primaries != P::Unspecified && cs.primaries != P::BT2020) {
        const char* tn = (cs.transfer == T::PQ) ? "pq" : "hlg";
        throw LoadError{ME_E_PARSE,
            where + ".colorSpace: transfer '" + tn +
            "' requires primaries 'bt2020' (HDR transfer functions are "
            "defined only over BT.2020 primaries)"};
    }

    /* R2: BT.2020NC matrix requires BT.2020 primaries. */
    if (cs.matrix == M::BT2020NC &&
        cs.primaries != P::Unspecified && cs.primaries != P::BT2020) {
        throw LoadError{ME_E_PARSE,
            where + ".colorSpace: matrix 'bt2020nc' requires primaries "
            "'bt2020' (matrix coefficients derive from primaries)"};
    }

    /* R3: Identity matrix (GBR) requires Full range. */
    if (cs.matrix == M::Identity &&
        cs.range != R::Unspecified && cs.range != R::Full) {
        throw LoadError{ME_E_PARSE,
            where + ".colorSpace: matrix 'identity' requires range "
            "'full' (identity matrix encodes GBR planes which are "
            "always full-range)"};
    }
}

me::ColorSpace parse_color_space(const json& j, const std::string& where) {
    require(j.is_object(), ME_E_PARSE, where + ".colorSpace: expected object");
    me::ColorSpace cs;
    if (j.contains("primaries")) cs.primaries = to_primaries(j["primaries"].get<std::string>());
    if (j.contains("transfer"))  cs.transfer  = to_transfer (j["transfer" ].get<std::string>());
    if (j.contains("matrix"))    cs.matrix    = to_matrix   (j["matrix"   ].get<std::string>());
    if (j.contains("range"))     cs.range     = to_range    (j["range"    ].get<std::string>());
    validate_color_space_combo(cs, where);
    return cs;
}

}  // namespace me::timeline_loader_detail
