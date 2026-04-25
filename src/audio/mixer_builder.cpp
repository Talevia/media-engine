/*
 * AudioMixer factory — walks a Timeline + per-clip DemuxContext list,
 * opens an AudioTrackFeed per audio clip, and forwards each clip's
 * gain_db to AudioMixer::add_track.
 *
 * Extracted from mixer.cpp (was 336 lines, near §1a's 400 ceiling)
 * so the mixer class itself stays focused on the per-frame pull
 * pipeline + so the next mixer feature (pan, sidechain, per-channel
 * gain) lands without crossing the threshold.
 *
 * The factory's public declaration stays in mixer.hpp; only the
 * implementation moves here. Callers are unchanged.
 */
#include "audio/mixer.hpp"

#include "audio/track_feed.hpp"
#include "timeline/timeline_impl.hpp"

#include <utility>

namespace me::audio {

me_status_t build_audio_mixer_for_timeline(
    const me::Timeline&                                         tl,
    me::resource::CodecPool&                                    pool,
    const std::vector<std::shared_ptr<me::io::DemuxContext>>&   demux_by_clip_idx,
    const AudioMixerConfig&                                     cfg,
    std::unique_ptr<AudioMixer>&                                out,
    std::string*                                                err) {

    if (demux_by_clip_idx.size() != tl.clips.size()) {
        if (err) *err = "build_audio_mixer_for_timeline: demux_by_clip_idx.size() "
                        "must equal tl.clips.size()";
        return ME_E_INVALID_ARG;
    }
    if (tl.clips.empty() || tl.tracks.empty()) {
        if (err) *err = "build_audio_mixer_for_timeline: timeline has no clips or tracks";
        return ME_E_INVALID_ARG;
    }

    /* Build track-kind lookup once (string → kind). Small N, linear
     * scan is fine but we'd hit it per-clip otherwise. */
    auto kind_for_track_id = [&](const std::string& track_id) {
        for (const auto& t : tl.tracks) {
            if (t.id == track_id) return t.kind;
        }
        return me::TrackKind::Video;   /* default harmless */
    };

    auto mixer = std::make_unique<AudioMixer>(cfg, err);
    if (!mixer->ok()) {
        return ME_E_INVALID_ARG;
    }

    std::size_t audio_clips_found = 0;
    for (std::size_t ci = 0; ci < tl.clips.size(); ++ci) {
        const me::Clip& c = tl.clips[ci];
        if (kind_for_track_id(c.track_id) != me::TrackKind::Audio) continue;
        if (!demux_by_clip_idx[ci]) {
            if (err) *err = "build_audio_mixer_for_timeline: null demux for audio clip idx " +
                             std::to_string(ci);
            return ME_E_INVALID_ARG;
        }

        AudioTrackFeed feed;
        const me_status_t s = open_audio_track_feed(
            demux_by_clip_idx[ci], pool,
            cfg.target_rate, cfg.target_fmt, cfg.target_ch_layout,
            feed, err);
        if (s != ME_OK) {
            /* Prefix with clip context for easier debugging. */
            if (err) *err = "build_audio_mixer_for_timeline: clip[" +
                             std::to_string(ci) + "] " + *err;
            return s;
        }

        me::AnimatedNumber gain_db = c.gain_db.has_value()
            ? *c.gain_db
            : me::AnimatedNumber::from_static(0.0);

        const me_status_t add_s = mixer->add_track(
            std::move(feed), std::move(gain_db), err);
        if (add_s != ME_OK) {
            if (err) *err = "build_audio_mixer_for_timeline: clip[" +
                             std::to_string(ci) + "] add_track: " + *err;
            return add_s;
        }
        ++audio_clips_found;
    }

    if (audio_clips_found == 0) {
        if (err) *err = "build_audio_mixer_for_timeline: no audio clips in timeline";
        return ME_E_NOT_FOUND;
    }

    out = std::move(mixer);
    return ME_OK;
}

}  // namespace me::audio
