/*
 * Shared keyframe-walker for `AnimatedNumber` / `AnimatedColor`
 * loader parsers. Both share schema-level structure — a `keyframes`
 * JSON array whose elements carry `t` (rational) + `v` (value, type
 * varies) + `interp` (enum) + optional `cp` (4-float CSS bezier
 * control points) + strict-sort-by-t post-check + strict unknown-key
 * rejection. The only divergence is how `v` is parsed
 * (double vs hex-string → RGBA bytes).
 *
 * Internal; not exposed via any public header. Templates live in
 * this `.hpp` so both translation units (loader_helpers_animated.cpp
 * + loader_helpers_clip_params.cpp) instantiate the shared body.
 */
#pragma once

#include "timeline/animated_number.hpp"   /* Interp enum */
#include "timeline/loader_helpers.hpp"    /* as_rational / require / LoadError */

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace me::timeline_loader_detail {

/* String → me::Interp. Shared by both animated-property loaders.
 * Unknown values throw LoadError with ME_E_PARSE. */
inline me::Interp parse_interp_from_string(const std::string& s,
                                             const std::string& where) {
    if (s == "linear")  return me::Interp::Linear;
    if (s == "bezier")  return me::Interp::Bezier;
    if (s == "hold")    return me::Interp::Hold;
    if (s == "stepped") return me::Interp::Stepped;
    throw LoadError{ME_E_PARSE,
        where + ".interp: unknown '" + s +
        "' (expected linear/bezier/hold/stepped)"};
}

/* Walk a `keyframes` JSON array and build a `std::vector<Kf>`.
 * `Kf` must expose `.t` (me_rational_t), `.v` (value type), `.interp`
 * (me::Interp), and `.cp` (std::array<double, 4>). `parse_v` is a
 * caller-supplied callable invoked on the per-keyframe `v` JSON
 * node + its contextual prefix; it returns the value assigned to
 * `kf.v`. Enforces:
 *   - Array is non-empty.
 *   - Each element is an object with t / v / interp keys.
 *   - Bezier keyframes carry a `cp` array of 4 numbers with
 *     x1 / x2 ∈ [0, 1].
 *   - No keys beyond {t, v, interp, cp}.
 *   - Strictly sorted by t (no duplicates / inversions).
 *
 * Returns the parsed vector. Throws LoadError on any violation. */
template <typename Kf, typename ValueParser>
std::vector<Kf> parse_keyframes_array(const nlohmann::json& arr,
                                        const std::string&    where,
                                        ValueParser           parse_v) {
    require(arr.is_array(), ME_E_PARSE, where + ": expected array");
    require(!arr.empty(), ME_E_PARSE,
            where + ": at least one keyframe required");

    std::vector<Kf> kfs;
    kfs.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& k  = arr[i];
        const std::string ki = where + "[" + std::to_string(i) + "]";
        require(k.is_object(), ME_E_PARSE, ki + ": expected object");
        require(k.contains("t"), ME_E_PARSE, ki + ".t: missing");
        require(k.contains("v"), ME_E_PARSE, ki + ".v: missing");
        require(k.contains("interp"), ME_E_PARSE, ki + ".interp: missing");

        Kf kf;
        kf.t = as_rational(k["t"], ki + ".t");
        kf.v = parse_v(k["v"], ki);
        require(k["interp"].is_string(), ME_E_PARSE,
                ki + ".interp: expected string");
        kf.interp = parse_interp_from_string(
            k["interp"].template get<std::string>(), ki);

        if (kf.interp == me::Interp::Bezier) {
            require(k.contains("cp"), ME_E_PARSE,
                    ki + ".cp: required when interp=bezier");
            const auto& cp = k["cp"];
            require(cp.is_array() && cp.size() == 4, ME_E_PARSE,
                    ki + ".cp: expected 4-element array");
            for (int j = 0; j < 4; ++j) {
                require(cp[j].is_number(), ME_E_PARSE,
                        ki + ".cp[" + std::to_string(j) + "]: expected number");
                kf.cp[j] = cp[j].template get<double>();
            }
            /* x1 / x2 must be in [0, 1] per CSS cubic-bezier. */
            require(kf.cp[0] >= 0.0 && kf.cp[0] <= 1.0, ME_E_PARSE,
                    ki + ".cp[0] (x1): must be in [0, 1]");
            require(kf.cp[2] >= 0.0 && kf.cp[2] <= 1.0, ME_E_PARSE,
                    ki + ".cp[2] (x2): must be in [0, 1]");
        }

        /* Strict unknown-key rejection — catches typos at load
         * time rather than silently dropping data. */
        for (auto it = k.begin(); it != k.end(); ++it) {
            const std::string& key = it.key();
            const bool is_known = (key == "t" || key == "v" ||
                                    key == "interp" || key == "cp");
            require(is_known, ME_E_PARSE,
                    ki + ": unknown keyframe key '" + key + "'");
        }

        kfs.push_back(std::move(kf));
    }

    /* Strict sort-by-t (no duplicates / inversions). */
    for (std::size_t i = 1; i < kfs.size(); ++i) {
        const me_rational_t pt = kfs[i - 1].t;
        const me_rational_t tt = kfs[i].t;
        const int64_t lhs = pt.num * tt.den;
        const int64_t rhs = tt.num * pt.den;
        require(lhs < rhs, ME_E_PARSE,
                where + ": must be strictly sorted by t (no duplicates, "
                "no inversion); issue at index " + std::to_string(i));
    }

    return kfs;
}

}  // namespace me::timeline_loader_detail
