/* mask_resolver impl. See header for the contract.
 *
 * Linear scan over `frames[]` finding the entry whose `t` is
 * closest to the requested `time` — same shape as
 * `landmark_resolver`. Closest-frame selection runs in pure
 * rational arithmetic.
 *
 * Base64 decode goes through libavutil's `av_base64_decode`
 * (LGPL, already in the link graph); the decoded bytes are
 * raw alpha (one uint8_t per pixel, row-major).
 */
#include "compose/mask_resolver.hpp"

extern "C" {
#include <libavutil/base64.h>
}

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

struct RationalDist {
    std::int64_t num = 0;
    std::int64_t den = 1;
};

RationalDist abs_diff(me_rational_t a, me_rational_t b) {
    const std::int64_t lhs = static_cast<std::int64_t>(a.num) *
                              static_cast<std::int64_t>(b.den);
    const std::int64_t rhs = static_cast<std::int64_t>(b.num) *
                              static_cast<std::int64_t>(a.den);
    const std::int64_t diff = lhs - rhs;
    const std::int64_t den_prod = static_cast<std::int64_t>(a.den) *
                                   static_cast<std::int64_t>(b.den);
    return RationalDist{ diff < 0 ? -diff : diff, den_prod };
}

bool less_than(const RationalDist& a, const RationalDist& b) {
    return a.num * b.den < b.num * a.den;
}

}  // namespace

me_status_t resolve_mask_alpha_from_file(
    std::string_view           file_uri,
    me_rational_t              time,
    int*                       out_width,
    int*                       out_height,
    std::vector<std::uint8_t>* out_alpha,
    std::string*               err) {

    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = "mask_resolver: " + std::move(msg);
        return s;
    };

    if (!out_width || !out_height || !out_alpha) {
        return fail(ME_E_INVALID_ARG, "out_* pointer is null");
    }
    if (file_uri.empty()) return fail(ME_E_INVALID_ARG, "file_uri is empty");

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
    *out_width  = 0;
    *out_height = 0;
    out_alpha->clear();
    if (frames.empty()) {
        return ME_OK;  /* legitimate "no mask available" representation */
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
                        "frames[" + std::to_string(i) + "].t.den must be > 0");
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

    /* Decode the best frame's mask. */
    const auto& best_frame = frames[best_idx];
    if (!best_frame.contains("width") || !best_frame.contains("height") ||
        !best_frame.contains("alphaB64") ||
        !best_frame["width"].is_number_integer() ||
        !best_frame["height"].is_number_integer() ||
        !best_frame["alphaB64"].is_string()) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "]: required {width, height, alphaB64} fields");
    }
    const int w = best_frame["width"].get<int>();
    const int h = best_frame["height"].get<int>();
    if (w <= 0 || h <= 0) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "]: width / height must be positive");
    }
    const std::string b64 = best_frame["alphaB64"].get<std::string>();
    const std::size_t expected_bytes =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    /* `av_base64_decode` requires the output buffer to be at least
     * 3/4 the input length. AV_BASE64_DECODE_SIZE rounds DOWN, so
     * over-allocate slightly for safety. */
    std::vector<std::uint8_t> alpha_buf(
        expected_bytes + 4);  /* +4 to absorb base64 padding rounding */
    int decoded = av_base64_decode(alpha_buf.data(), b64.c_str(),
                                    static_cast<int>(alpha_buf.size()));
    if (decoded < 0) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "].alphaB64: base64 decode failed");
    }
    if (static_cast<std::size_t>(decoded) != expected_bytes) {
        return fail(ME_E_PARSE,
                    "frames[" + std::to_string(best_idx) +
                    "].alphaB64: decoded " + std::to_string(decoded) +
                    " bytes but expected " + std::to_string(expected_bytes) +
                    " (width*height)");
    }
    alpha_buf.resize(expected_bytes);
    *out_width  = w;
    *out_height = h;
    *out_alpha  = std::move(alpha_buf);
    return ME_OK;
}

}  // namespace me::compose
