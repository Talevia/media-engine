#include "orchestrator/compose_audio.hpp"

#include "orchestrator/reencode_audio.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/frame.h>
}

namespace me::orchestrator {

me_status_t setup_compose_audio_mixer(
    const me::Timeline&                                       tl,
    const std::vector<std::shared_ptr<me::io::DemuxContext>>& demuxes,
    const detail::SharedEncState&                             shared,
    me::resource::CodecPool*                                  pool,
    std::unique_ptr<me::audio::AudioMixer>&                   out_mixer,
    std::string*                                              err) {

    out_mixer.reset();
    bool has_audio_tracks = false;
    for (const auto& t : tl.tracks) {
        if (t.kind == me::TrackKind::Audio) { has_audio_tracks = true; break; }
    }
    if (!(has_audio_tracks && shared.aenc && pool)) {
        return ME_OK;   /* no-op: no audio tracks, no encoder, or no pool */
    }

    me::audio::AudioMixerConfig mix_cfg;
    mix_cfg.target_rate = shared.aenc->sample_rate;
    mix_cfg.target_fmt  = shared.aenc->sample_fmt;
    if (av_channel_layout_copy(&mix_cfg.target_ch_layout,
                                &shared.aenc->ch_layout) < 0) {
        if (err) *err = "setup_compose_audio_mixer: channel layout copy";
        return ME_E_INTERNAL;
    }
    mix_cfg.frame_size = shared.aenc->frame_size > 0
        ? shared.aenc->frame_size : 1024;
    mix_cfg.peak_threshold = 0.95f;

    std::unique_ptr<me::audio::AudioMixer> mixer;
    const me_status_t build_s = me::audio::build_audio_mixer_for_timeline(
        tl, *pool, demuxes, mix_cfg, mixer, err);
    av_channel_layout_uninit(&mix_cfg.target_ch_layout);
    if (build_s == ME_E_NOT_FOUND) {
        /* Timeline declared audio tracks but no audio clips — fall
         * through to video-only audio flush. */
        return ME_OK;
    }
    if (build_s != ME_OK) return build_s;

    out_mixer = std::move(mixer);
    return ME_OK;
}

me_status_t drain_compose_audio(
    me::audio::AudioMixer*      mixer,
    detail::SharedEncState&     shared,
    std::string*                err) {

    if (!shared.aenc) return ME_OK;   /* no audio encoder → nothing to drain */

    if (mixer) {
        while (true) {
            AVFrame* mf = nullptr;
            const me_status_t ps = mixer->pull_next_mixed_frame(&mf, err);
            if (ps == ME_E_NOT_FOUND) break;
            if (ps != ME_OK) return ps;

            mf->pts = shared.next_audio_pts;
            shared.next_audio_pts += mf->nb_samples;
            const me_status_t es = detail::encode_audio_frame(
                mf, shared.aenc, shared.ofmt, shared.out_aidx, err);
            av_frame_free(&mf);
            if (es != ME_OK) return es;
        }
    } else {
        if (auto s = detail::drain_audio_fifo(
                shared.afifo, shared.aenc, shared.ofmt,
                shared.out_aidx, &shared.next_audio_pts,
                /*flush=*/true, err);
            s != ME_OK) {
            return s;
        }
    }
    /* Common encoder flush. */
    return detail::encode_audio_frame(
        nullptr, shared.aenc, shared.ofmt, shared.out_aidx, err);
}

}  // namespace me::orchestrator
