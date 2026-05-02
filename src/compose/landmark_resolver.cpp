/* landmark_resolver impl. See header for the contract.
 *
 * Single-pass linear scan over `frames[]` finding the entry whose
 * `t` is closest to the requested `time`. Distance is
 * `|t_frame - time|` in rational arithmetic — no double-precision
 * intermediate, no cross-host SIMD drift. Ties (equidistant
 * frames) pick the first one in document order; callers shouldn't
 * rely on tie-breaking semantics (synthetic fixtures don't have
 * duplicate timestamps).
 */
#include "compose/landmark_resolver.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace me::compose {

namespace {

using json = nlohmann::json;

std::string strip_file_scheme(std::string_view uri) {
    constexpr std::string_view kFile = "file://";
    if (uri.starts_with(kFile)) return std::string(uri.substr(kFile.size()));
    return std::string(uri);
}

/* Absolute distance |a - b| as a rational. To compare two
 * distances, we cross-multiply: (a/b) < (c/d) iff a*d < c*b
 * (both b, d > 0). Caller compares
 * `dist_lhs.num * dist_rhs.den < dist_rhs.num * dist_lhs.den`. */
struct RationalDist {
    std::int64_t num = 0;
    std::int64_t den = 1;
};

RationalDist abs_diff(me_rational_t a, me_rational_t b) {
    /* a/b - c/d == (a*d - c*b) / (b*d). We want the absolute. */
    const std::int64_t lhs = static_cast<std::int64_t>(a.num) *
                              static_cast<std::int64_t>(b.den);
    const std::int64_t rhs = static_cast<std::int64_t>(b.num) *
                              static_cast<std::int64_t>(a.den);
    const std::int64_t diff = lhs - rhs;
    const std::int64_t den_prod = static_cast<std::int64_t>(a.den) *
                                   static_cast<std::int64_t>(b.den);
    return RationalDist{
        diff < 0 ? -diff : diff,
        den_prod
    };
}

bool less_than(const RationalDist& a, const RationalDist& b) {
    /* a.num/a.den < b.num/b.den iff a.num*b.den < b.num*a.den
     * (both denominators positive). */
    return a.num * b.den < b.num * a.den;
}

}  // namespace

me_status_t resolve_landmark_bboxes_from_file(
    std::string_view  file_uri,
    me_rational_t     time,
    std::vector<Bbox>* out,
    std::string*      err) {

    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = "landmark_resolver: " + std::move(msg);
        return s;
    };

    if (!out) return fail(ME_E_INVALID_ARG, "out is null");
    if (file_uri.empty()) return fail(ME_E_INVALID_ARG, "file_uri is empty");

    /* URI shape check — same shapes accepted as
     * `decode_sticker_to_rgba8` for consistency. */
    auto is_supported_scheme = [](std::string_view u) {
        if (u.starts_with("file://")) return true;
        if (u.starts_with("/"))       return true;
        if (u.starts_with("./") || u.starts_with("../")) return true;
        if (u.find("://") == std::string_view::npos) return true;
        return false;
    };
    if (!is_supported_scheme(file_uri)) {
        return fail(ME_E_UNSUPPORTED,
                    "uri scheme not supported (got '" + std::string(file_uri) +
                    "', expected file:// or path)");
    }

    const std::string path = strip_file_scheme(file_uri);

    std::ifstream f(path);
    if (!f.is_open()) {
        return fail(ME_E_IO, "open '" + path + "': failed");
    }
    std::stringstream buf;
    buf << f.rdbuf();
    if (f.bad()) {
        return fail(ME_E_IO, "read '" + path + "': failed");
    }

    json j;
    try {
        j = json::parse(buf.str());
    } catch (const json::parse_error& e) {
        return fail(ME_E_PARSE, "json parse '" + path + "': " + e.what());
    }

    if (!j.is_object() || !j.contains("frames") || !j["frames"].is_array()) {
        return fail(ME_E_PARSE,
                    "json '" + path + "': required object with 'frames' array");
    }

    const auto& frames = j["frames"];
    out->clear();
    if (frames.empty()) {
        /* No frames at all → no bboxes; legitimate "no landmarks
         * detected anywhere" representation. */
        return ME_OK;
    }

    /* Linear scan for the frame closest to `time`. */
    std::size_t best_idx = 0;
    RationalDist best_dist;
    bool         have_best = false;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& fr = frames[i];
        if (!fr.is_object() || !fr.contains("t") || !fr["t"].is_object()) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(i) + "]: missing 't' object");
        }
        const auto& t_obj = fr["t"];
        if (!t_obj.contains("num") || !t_obj.contains("den") ||
            !t_obj["num"].is_number_integer() || !t_obj["den"].is_number_integer()) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(i) +
                        "].t: required {num, den} integer object");
        }
        const std::int64_t den = t_obj["den"].get<std::int64_t>();
        if (den <= 0) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(i) +
                        "].t.den must be > 0");
        }
        me_rational_t fr_t{
            t_obj["num"].get<std::int64_t>(), den
        };
        RationalDist dist = abs_diff(fr_t, time);
        if (!have_best || less_than(dist, best_dist)) {
            best_dist = dist;
            best_idx  = i;
            have_best = true;
        }
    }

    /* Decode the best frame's bboxes. */
    const auto& best_frame = frames[best_idx];
    if (!best_frame.contains("bboxes") || !best_frame["bboxes"].is_array()) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "]: missing 'bboxes' array");
    }
    const auto& bboxes = best_frame["bboxes"];
    out->reserve(bboxes.size());
    for (std::size_t i = 0; i < bboxes.size(); ++i) {
        const auto& b = bboxes[i];
        if (!b.is_object() ||
            !b.contains("x0") || !b.contains("y0") ||
            !b.contains("x1") || !b.contains("y1") ||
            !b["x0"].is_number_integer() || !b["y0"].is_number_integer() ||
            !b["x1"].is_number_integer() || !b["y1"].is_number_integer()) {
            return fail(ME_E_PARSE,
                        "frames[" + std::to_string(best_idx) +
                        "].bboxes[" + std::to_string(i) +
                        "]: required {x0, y0, x1, y1} integer object");
        }
        Bbox bb;
        bb.x0 = b["x0"].get<std::int32_t>();
        bb.y0 = b["y0"].get<std::int32_t>();
        bb.x1 = b["x1"].get<std::int32_t>();
        bb.y1 = b["y1"].get<std::int32_t>();
        out->push_back(bb);
    }
    return ME_OK;
}

}  // namespace me::compose
