/*
 * Timeline JSON → IR parse helpers (shared by timeline_loader.cpp).
 *
 * Scope-A slice of `debt-split-timeline-loader-cpp`. Extracted from
 * timeline_loader.cpp to drop that TU below the 400-line debt
 * threshold. Covers:
 *   - `LoadError` exception-carrying struct.
 *   - Primitive parsers: `as_rational`, `require`, `rational_eq`.
 *   - ColorSpace enum-table shims: `to_primaries`, `to_transfer`,
 *     `to_matrix`, `to_range`.
 *   - Shape parsers: `parse_animated_static_number`,
 *     `parse_transform`, `parse_color_space`.
 *
 * Internal: namespace me::timeline_loader_detail. Not exposed via
 * any public header. timeline_loader.cpp does a `using namespace`
 * at its anon-namespace scope to keep call sites unqualified.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/animated_number.hpp"
#include "timeline/timeline_impl.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace me::timeline_loader_detail {

struct LoadError {
    me_status_t status;
    std::string message;
};

me_rational_t as_rational(const nlohmann::json& j, std::string_view field);
void          require(bool cond, me_status_t s, std::string msg);
bool          rational_eq(me_rational_t a, me_rational_t b);

me::ColorSpace::Primaries to_primaries(const std::string& s);
me::ColorSpace::Transfer  to_transfer (const std::string& s);
me::ColorSpace::Matrix    to_matrix   (const std::string& s);
me::ColorSpace::Range     to_range    (const std::string& s);

/* Parse a `{"static": <number>}` animated-property wrapper into a
 * plain double. Phase-1 rejects `{"keyframes": [...]}` with
 * ME_E_UNSUPPORTED; transform-animated-support bullet handles that
 * form when lifted to AnimatedNumber. */
double          parse_animated_static_number(const nlohmann::json& prop,
                                              const std::string& where);

/* Parse a `{"static": <number>}` OR `{"keyframes": [...]}` wrapper
 * into `me::AnimatedNumber`. Full schema per TIMELINE_SCHEMA.md
 * §Animated Properties: keyframes sorted by t, no duplicates,
 * interp ∈ {linear, bezier, hold, stepped}, bezier requires `cp`
 * (4-float array with x1/x2 ∈ [0,1]).
 *
 * Consumed by `parse_transform` (all 8 Transform fields) and
 * `Clip::gain_db` parsing. */
me::AnimatedNumber parse_animated_number(const nlohmann::json& prop,
                                          const std::string& where);

me::Transform   parse_transform   (const nlohmann::json& j, const std::string& where);
me::ColorSpace  parse_color_space (const nlohmann::json& j, const std::string& where);

}  // namespace me::timeline_loader_detail
