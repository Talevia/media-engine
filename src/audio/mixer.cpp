#include "audio/mixer.hpp"

#include "audio/mix.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstring>

namespace me::audio {

AudioMixer::AudioMixer(const AudioMixerConfig& cfg, std::string* err)
    : cfg_(cfg) {
    /* Explicit copy of channel layout so the caller's AVChannelLayout
     * lifetime doesn't affect us. */
    av_channel_layout_uninit(&cfg_.target_ch_layout);
    if (av_channel_layout_copy(&cfg_.target_ch_layout, &cfg.target_ch_layout) < 0) {
        if (err) *err = "AudioMixer: av_channel_layout_copy failed";
        return;
    }
    if (cfg_.target_fmt != AV_SAMPLE_FMT_FLTP) {
        if (err) *err = "AudioMixer: phase-1 requires AV_SAMPLE_FMT_FLTP target_fmt";
        return;
    }
    if (cfg_.target_rate <= 0 || cfg_.frame_size <= 0) {
        if (err) *err = "AudioMixer: target_rate and frame_size must be > 0";
        return;
    }
    num_channels_ = cfg_.target_ch_layout.nb_channels;
    if (num_channels_ <= 0) {
        if (err) *err = "AudioMixer: target_ch_layout.nb_channels must be > 0";
        return;
    }
    ok_ = true;
}

AudioMixer::~AudioMixer() {
    av_channel_layout_uninit(&cfg_.target_ch_layout);
}

me_status_t AudioMixer::add_track(AudioTrackFeed     feed,
                                   me::AnimatedNumber gain_db,
                                   std::string*       err) {
    if (!ok_) {
        if (err) *err = "AudioMixer: mixer not ok (check ctor err)";
        return ME_E_INTERNAL;
    }
    if (feed.target_rate != cfg_.target_rate ||
        feed.target_fmt  != cfg_.target_fmt  ||
        feed.target_ch_layout.nb_channels != num_channels_) {
        if (err) *err = "AudioMixer::add_track: feed target params don't match mixer config";
        return ME_E_INVALID_ARG;
    }

    TrackState ts;
    ts.feed    = std::move(feed);
    ts.gain_db = std::move(gain_db);
    ts.planes.assign(num_channels_, std::vector<float>{});
    tracks_.push_back(std::move(ts));
    return ME_OK;
}

me_status_t AudioMixer::fill_one_from_feed(TrackState& ts, std::string* err) {
    if (ts.feed.eof) return ME_E_NOT_FOUND;

    AVFrame* f = nullptr;
    const me_status_t s = pull_next_processed_audio_frame(ts.feed, &f, err);
    if (s == ME_E_NOT_FOUND) return ME_E_NOT_FOUND;
    if (s != ME_OK) return s;
    if (!f) {
        if (err) *err = "AudioMixer::fill_one_from_feed: pull returned ME_OK but null frame";
        return ME_E_INTERNAL;
    }

    /* Feed returns FLTP at target (rate, ch_layout) — append per-plane
     * samples to our per-plane FIFOs. */
    const int nb = f->nb_samples;
    if (f->ch_layout.nb_channels != num_channels_) {
        av_frame_free(&f);
        if (err) *err = "AudioMixer::fill_one_from_feed: feed produced channel count != mixer config";
        return ME_E_INTERNAL;
    }
    for (int ch = 0; ch < num_channels_; ++ch) {
        auto* src = reinterpret_cast<const float*>(f->extended_data[ch]);
        auto& dst = ts.planes[ch];
        dst.insert(dst.end(), src, src + nb);
    }
    av_frame_free(&f);
    return ME_OK;
}

bool AudioMixer::eof() const noexcept {
    for (const auto& ts : tracks_) {
        if (!ts.feed.eof) return false;
        for (const auto& p : ts.planes) {
            if (!p.empty()) return false;
        }
    }
    return true;
}

me_status_t AudioMixer::pull_next_mixed_frame(AVFrame**    out_frame,
                                               std::string* err) {
    if (!out_frame) {
        if (err) *err = "AudioMixer::pull_next_mixed_frame: null out_frame";
        return ME_E_INVALID_ARG;
    }
    *out_frame = nullptr;
    if (!ok_) {
        if (err) *err = "AudioMixer::pull_next_mixed_frame: mixer not ok";
        return ME_E_INTERNAL;
    }
    if (tracks_.empty()) {
        if (err) *err = "AudioMixer::pull_next_mixed_frame: no tracks added";
        return ME_E_INVALID_ARG;
    }

    const int want = cfg_.frame_size;

    /* Ensure each track has >= want samples in its plane[0] FIFO,
     * or the feed is EOF (in which case we'll pad with silence). */
    for (auto& ts : tracks_) {
        while (!ts.feed.eof &&
               static_cast<int>(ts.planes.front().size()) < want) {
            const me_status_t fill_s = fill_one_from_feed(ts, err);
            if (fill_s == ME_E_NOT_FOUND) break;  /* feed eof set; move on */
            if (fill_s != ME_OK) return fill_s;
        }
    }

    /* Full EOF check — all feeds drained and all FIFOs short. */
    if (eof()) return ME_E_NOT_FOUND;

    /* Build per-channel input arrays. Per CLAUDE.md anti-requirement
     * #7 (no std::unordered_map), use a deterministic per-track
     * iteration order (tracks_.push_back order). */
    const int N = static_cast<int>(tracks_.size());

    /* Track contribution buffers: for each channel, one strip of
     * `want` samples per track. Tracks that have fewer than `want`
     * samples in their FIFO (due to feed EOF) get zero-padding for
     * the shortfall. */
    std::vector<std::vector<float>> track_strips_buf;
    track_strips_buf.assign(N, std::vector<float>(want, 0.0f));

    /* Evaluate per-track animated gain at this frame's timeline T.
     * T is the emission cursor in rational form — samples_emitted_
     * is monotonically advanced by `want` each successful pull, so
     * the same T is produced for the same frame index across runs
     * (determinism). Per-frame-constant gain is a buffer-level
     * approximation; within a 1024/48k = ~21ms window we hold gain
     * flat rather than interpolate per-sample. */
    const me_rational_t T{samples_emitted_, cfg_.target_rate};
    std::vector<float> gains(N, 1.0f);
    for (int i = 0; i < N; ++i) {
        gains[i] = db_to_linear(
            static_cast<float>(tracks_[i].gain_db.evaluate_at(T)));
    }

    /* Output frame allocated up front; planes filled per channel. */
    AVFrame* out = av_frame_alloc();
    if (!out) {
        if (err) *err = "AudioMixer::pull_next_mixed_frame: av_frame_alloc failed";
        return ME_E_OUT_OF_MEMORY;
    }
    out->format      = cfg_.target_fmt;
    out->sample_rate = cfg_.target_rate;
    if (av_channel_layout_copy(&out->ch_layout, &cfg_.target_ch_layout) < 0) {
        av_frame_free(&out);
        if (err) *err = "AudioMixer::pull_next_mixed_frame: av_channel_layout_copy failed";
        return ME_E_INTERNAL;
    }
    out->nb_samples = want;
    if (av_frame_get_buffer(out, 0) < 0) {
        av_frame_free(&out);
        if (err) *err = "AudioMixer::pull_next_mixed_frame: av_frame_get_buffer failed";
        return ME_E_OUT_OF_MEMORY;
    }

    for (int ch = 0; ch < num_channels_; ++ch) {
        /* Per-track strip fill: copy up to `want` samples from each
         * track's FIFO plane[ch]; if FIFO has less (feed drained and
         * residual < want), zero-pad the tail. */
        std::vector<const float*> inputs(N, nullptr);
        for (int i = 0; i < N; ++i) {
            auto& plane_q = tracks_[i].planes[ch];
            const int have = static_cast<int>(plane_q.size());
            const int take = std::min(want, have);
            if (take > 0) {
                std::memcpy(track_strips_buf[i].data(),
                            plane_q.data(),
                            static_cast<std::size_t>(take) * sizeof(float));
            }
            if (take < want) {
                std::memset(track_strips_buf[i].data() + take, 0,
                            static_cast<std::size_t>(want - take) * sizeof(float));
            }
            inputs[i] = track_strips_buf[i].data();
        }

        auto* out_plane = reinterpret_cast<float*>(out->extended_data[ch]);
        mix_samples(inputs.data(), gains.data(),
                    static_cast<std::size_t>(N),
                    static_cast<std::size_t>(want),
                    out_plane);
        peak_limiter(out_plane, static_cast<std::size_t>(want),
                     cfg_.peak_threshold);
    }

    /* Pop `want` samples (or however many were available) from each
     * track's plane FIFOs. erase from begin() — O(want) cost per
     * pop, but mix windows are small (1024) relative to typical
     * audio-track length; if this becomes hot, swap for a ring
     * buffer in a follow-up cycle. */
    for (auto& ts : tracks_) {
        for (auto& plane_q : ts.planes) {
            const int have = static_cast<int>(plane_q.size());
            const int take = std::min(want, have);
            plane_q.erase(plane_q.begin(), plane_q.begin() + take);
        }
    }

    samples_emitted_ += want;
    *out_frame = out;
    return ME_OK;
}

me_status_t AudioMixer::inject_samples_for_test(
    std::size_t         ti,
    const float* const* plane_data,
    std::size_t         num_samples,
    std::string*        err) {
    if (!ok_) {
        if (err) *err = "AudioMixer::inject_samples_for_test: mixer not ok";
        return ME_E_INTERNAL;
    }
    if (ti >= tracks_.size() || !plane_data) {
        if (err) *err = "AudioMixer::inject_samples_for_test: bad track index or null plane_data";
        return ME_E_INVALID_ARG;
    }
    auto& planes = tracks_[ti].planes;
    if (static_cast<int>(planes.size()) != num_channels_) {
        if (err) *err = "AudioMixer::inject_samples_for_test: internal channel count mismatch";
        return ME_E_INTERNAL;
    }
    for (int ch = 0; ch < num_channels_; ++ch) {
        if (!plane_data[ch]) {
            if (err) *err = "AudioMixer::inject_samples_for_test: null plane pointer";
            return ME_E_INVALID_ARG;
        }
        planes[ch].insert(planes[ch].end(), plane_data[ch], plane_data[ch] + num_samples);
    }
    return ME_OK;
}

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
