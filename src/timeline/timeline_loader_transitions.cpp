/* parse_track_transitions_into — transitions-array walker.
 *
 * Scope-B slice of debt-split-timeline-loader-cpp. Phase-1 schema
 * accepts only `crossDissolve`; other kinds surface as
 * ME_E_UNSUPPORTED. Cross-track adjacency + duration bounding is
 * enforced:
 *   - fromClipId / toClipId both exist in this track's clip id list.
 *   - toClipId immediately follows fromClipId in track's JSON order.
 *   - duration > 0 and ≤ min(from.time_duration, to.time_duration).
 *
 * Consumed by cross-dissolve-kernel (M2 follow-up).
 */
#include "timeline/loader_helpers.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

void parse_track_transitions_into(
    const json&                                             transitions,
    const std::string&                                      track_id,
    const std::string&                                      track_where,
    const std::vector<std::string>&                         track_clip_ids,
    const std::unordered_map<std::string, me_rational_t>&   clip_dur_by_id,
    me::Timeline&                                           tl) {

    require(transitions.is_array(), ME_E_PARSE,
            track_where + ".transitions: expected array");

    for (std::size_t i = 0; i < transitions.size(); ++i) {
        const auto& tr_j = transitions[i];
        const std::string trw =
            track_where + ".transitions[" + std::to_string(i) + "]";
        require(tr_j.is_object(), ME_E_PARSE, trw + ": expected object");

        const std::string kind_str = tr_j.at("kind").get<std::string>();
        require(kind_str == "crossDissolve", ME_E_UNSUPPORTED,
                trw + ".kind: only 'crossDissolve' supported in phase-1 "
                "(got '" + kind_str + "')");

        const std::string from_id = tr_j.at("fromClipId").get<std::string>();
        const std::string to_id   = tr_j.at("toClipId").get<std::string>();
        require(!from_id.empty() && !to_id.empty(), ME_E_PARSE,
                trw + ": fromClipId / toClipId must be non-empty");

        const auto from_it = clip_dur_by_id.find(from_id);
        const auto to_it   = clip_dur_by_id.find(to_id);
        require(from_it != clip_dur_by_id.end(), ME_E_PARSE,
                trw + ".fromClipId refers to unknown clip '" + from_id +
                "' in this track");
        require(to_it != clip_dur_by_id.end(), ME_E_PARSE,
                trw + ".toClipId refers to unknown clip '" + to_id +
                "' in this track");

        /* Adjacency: find from's index in the ordered list and
         * check to follows immediately. */
        std::size_t from_idx = 0;
        for (; from_idx < track_clip_ids.size(); ++from_idx) {
            if (track_clip_ids[from_idx] == from_id) break;
        }
        require(from_idx + 1 < track_clip_ids.size() &&
                    track_clip_ids[from_idx + 1] == to_id,
                ME_E_PARSE,
                trw + ": toClipId must immediately follow fromClipId "
                "in this track's clips array");

        const me_rational_t dur =
            as_rational(tr_j.at("duration"), trw + ".duration");
        require(dur.num > 0, ME_E_PARSE, trw + ".duration must be positive");

        auto le_clip_dur = [&](me_rational_t cd) {
            return dur.num * cd.den <= cd.num * dur.den;
        };
        require(le_clip_dur(from_it->second) && le_clip_dur(to_it->second),
                ME_E_PARSE,
                trw + ".duration must not exceed either adjacent "
                "clip's duration");

        tl.transitions.push_back(me::Transition{
            me::TransitionKind::CrossDissolve,
            track_id,
            from_id,
            to_id,
            dur,
        });
    }
}

}  // namespace me::timeline_loader_detail
