/* parse_track_into — single-track walker (clips + transitions + metadata).
 *
 * Scope-B slice of debt-split-timeline-loader-cpp. This is the bulk
 * of the original load_json's per-track body (lines ~132-395 in the
 * pre-split file). Delegates the transitions pass to
 * parse_track_transitions_into (in timeline_loader_transitions.cpp)
 * so that subfunction's ~80 lines live on their own.
 *
 * Phase-1 within-track invariants enforced here:
 *   - Non-empty `clips` array.
 *   - Per-clip `type` matches parent track.kind.
 *   - Synthetic clip types (text / subtitle) may omit assetId /
 *     sourceRange; video / audio must supply both.
 *   - timeRange.start equals cumulative prior clip duration.
 *   - timeRange.duration == sourceRange.duration (phase-1).
 *   - transform / gainDb / effects / textParams / subtitleParams
 *     each restricted to their compatible track kinds.
 *   - Clip ids unique within the track (for transition resolution).
 */
#include "timeline/loader_helpers.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me::timeline_loader_detail {

using json = nlohmann::json;

namespace {

auto rational_gt = [](me_rational_t a, me_rational_t b) {
    return a.num * b.den > b.num * a.den;
};

}  // namespace

void parse_track_into(const json&                                    track,
                      std::size_t                                    track_idx,
                      me::Timeline&                                  tl,
                      me_rational_t&                                 max_duration_out,
                      std::unordered_map<std::string, std::size_t>&  track_id_seen) {

    const std::string track_where = "track[" + std::to_string(track_idx) + "]";

    require(track.contains("id"), ME_E_PARSE, track_where + ": missing 'id'");
    const std::string track_id = track.at("id").get<std::string>();
    require(!track_id.empty(), ME_E_PARSE, track_where + ": 'id' must be non-empty");
    require(track_id_seen.emplace(track_id, track_idx).second, ME_E_PARSE,
            track_where + ": duplicate track id '" + track_id + "'");

    const std::string track_kind_str = track.at("kind").get<std::string>();
    me::TrackKind track_kind;
    if      (track_kind_str == "video")    track_kind = me::TrackKind::Video;
    else if (track_kind_str == "audio")    track_kind = me::TrackKind::Audio;
    else if (track_kind_str == "text")     track_kind = me::TrackKind::Text;
    else if (track_kind_str == "subtitle") track_kind = me::TrackKind::Subtitle;
    else {
        throw LoadError{ME_E_UNSUPPORTED,
            track_where + ".kind: only 'video', 'audio', 'text', 'subtitle' "
            "supported (got '" + track_kind_str + "')"};
    }
    const char* expected_clip_type =
        (track_kind == me::TrackKind::Video)    ? "video" :
        (track_kind == me::TrackKind::Audio)    ? "audio" :
        (track_kind == me::TrackKind::Text)     ? "text"  :
                                                   "subtitle";

    const bool track_enabled =
        track.contains("enabled") ? track.at("enabled").get<bool>() : true;

    const auto& clips = track.at("clips");
    require(!clips.empty(), ME_E_PARSE, track_where + ": clips must have at least one clip");

    me_rational_t running{0, 1};
    std::vector<std::string> track_clip_ids;
    std::unordered_map<std::string, me_rational_t> clip_dur_by_id;
    track_clip_ids.reserve(clips.size());

    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& clip = clips[i];
        const std::string where = track_where + ".clip[" + std::to_string(i) + "]";

        const std::string clip_type = clip.at("type").get<std::string>();
        require(clip_type == expected_clip_type, ME_E_PARSE,
                where + ".type: must match parent track.kind (expected '" +
                expected_clip_type + "', got '" + clip_type + "')");

        std::string asset_id;
        if (clip.contains("assetId")) {
            asset_id = clip.at("assetId").get<std::string>();
        }
        const bool synthetic_clip =
            track_kind == me::TrackKind::Text ||
            track_kind == me::TrackKind::Subtitle;
        if (synthetic_clip) {
            if (!asset_id.empty() && tl.assets.find(asset_id) == tl.assets.end()) {
                asset_id.clear();
            }
        } else {
            require(!asset_id.empty(), ME_E_PARSE,
                    where + ": '" + clip_type + "' clip requires 'assetId'");
            require(tl.assets.find(asset_id) != tl.assets.end(), ME_E_PARSE,
                    where + ".assetId refers to unknown asset");
        }

        const auto& tr = clip.at("timeRange");
        me_rational_t t_start = as_rational(tr.at("start"),    where + ".timeRange.start");
        me_rational_t t_dur   = as_rational(tr.at("duration"), where + ".timeRange.duration");

        me_rational_t s_start{0, 1};
        me_rational_t s_dur   = t_dur;
        if (clip.contains("sourceRange")) {
            const auto& sr = clip.at("sourceRange");
            s_start = as_rational(sr.at("start"),    where + ".sourceRange.start");
            s_dur   = as_rational(sr.at("duration"), where + ".sourceRange.duration");
        } else {
            require(synthetic_clip, ME_E_PARSE,
                    where + ": 'sourceRange' is required on non-text / "
                    "non-subtitle clips");
        }

        require(rational_eq(t_start, running), ME_E_UNSUPPORTED,
                where + ".timeRange.start must equal cumulative prior clip duration "
                "within this track (phase-1: no within-track gaps or overlaps)");
        require(rational_eq(t_dur, s_dur), ME_E_UNSUPPORTED,
                where + ": phase-1: timeRange.duration must equal sourceRange.duration");
        require(t_dur.num > 0, ME_E_PARSE, where + ".timeRange.duration must be positive");
        require(s_start.num >= 0, ME_E_PARSE, where + ".sourceRange.start must be >= 0");

        me::Clip c;
        c.asset_id      = asset_id;
        c.track_id      = track_id;
        c.type          = (track_kind == me::TrackKind::Video)    ? me::ClipType::Video    :
                          (track_kind == me::TrackKind::Audio)    ? me::ClipType::Audio    :
                          (track_kind == me::TrackKind::Text)     ? me::ClipType::Text     :
                                                                     me::ClipType::Subtitle;
        c.time_start    = t_start;
        c.time_duration = t_dur;
        c.source_start  = s_start;
        if (clip.contains("transform")) {
            /* Transform is a 2D positional concept — meaningful for
             * any visual clip kind (video / text / subtitle). Audio
             * clips reject it since there's no spatial axis. */
            require(track_kind != me::TrackKind::Audio, ME_E_PARSE,
                    where + ".transform: not valid on audio clip (2D positional "
                    "transform is meaningless for audio)");
            c.transform = parse_transform(clip["transform"], where + ".transform");
        }
        if (clip.contains("gainDb")) {
            require(track_kind == me::TrackKind::Audio, ME_E_PARSE,
                    where + ".gainDb: not valid on video clip (audio gain is "
                    "only meaningful for audio clips)");
            c.gain_db = parse_animated_number(clip["gainDb"], where + ".gainDb");
        }
        if (clip.contains("effects")) {
            require(track_kind == me::TrackKind::Video, ME_E_PARSE,
                    where + ".effects: not valid on audio clip (audio "
                    "effect chain lands with M4 audio polish)");
            const auto& eff_arr = clip["effects"];
            require(eff_arr.is_array(), ME_E_PARSE,
                    where + ".effects: expected array");
            for (std::size_t ei = 0; ei < eff_arr.size(); ++ei) {
                c.effects.push_back(parse_effect_spec(
                    eff_arr[ei],
                    where + ".effects[" + std::to_string(ei) + "]"));
            }
        }
        if (track_kind == me::TrackKind::Text) {
            require(clip.contains("textParams"), ME_E_PARSE,
                    where + ": text clip requires 'textParams'");
            c.text_params = parse_text_clip_params(
                clip["textParams"], where + ".textParams");
        } else {
            require(!clip.contains("textParams"), ME_E_PARSE,
                    where + ".textParams: only valid on text clips");
        }
        if (track_kind == me::TrackKind::Subtitle) {
            require(clip.contains("subtitleParams"), ME_E_PARSE,
                    where + ": subtitle clip requires 'subtitleParams'");
            c.subtitle_params = parse_subtitle_clip_params(
                clip["subtitleParams"], where + ".subtitleParams");
        } else {
            require(!clip.contains("subtitleParams"), ME_E_PARSE,
                    where + ".subtitleParams: only valid on subtitle clips");
        }
        const std::string clip_id = clip.at("id").get<std::string>();
        require(!clip_id.empty(), ME_E_PARSE, where + ".id must be non-empty");
        require(clip_dur_by_id.emplace(clip_id, t_dur).second, ME_E_PARSE,
                where + ".id: duplicate clip id '" + clip_id +
                "' within this track");
        track_clip_ids.push_back(clip_id);
        c.id = clip_id;
        tl.clips.push_back(std::move(c));

        /* running += t_dur in rational: a/b + c/d = (a*d + c*b) / (b*d). */
        running = me_rational_t{
            running.num * t_dur.den + t_dur.num * running.den,
            running.den * t_dur.den
        };
    }

    if (track.contains("transitions")) {
        parse_track_transitions_into(track["transitions"], track_id, track_where,
                                       track_clip_ids, clip_dur_by_id, tl);
    }

    tl.tracks.push_back(me::Track{track_id, track_kind, track_enabled});
    if (rational_gt(running, max_duration_out)) max_duration_out = running;
}

}  // namespace me::timeline_loader_detail
