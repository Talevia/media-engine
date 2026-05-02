#include "orchestrator/audio_only_sink.hpp"

#include "audio/mixer.hpp"
#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/codec_resolver.hpp"
#include "orchestrator/encoder_mux_setup.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_pipeline.hpp"
#include "orchestrator/reencode_segment.hpp"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/frame.h>
}

#include <cstring>
#include <utility>

namespace me::orchestrator {

namespace {

class AudioOnlySink final : public OutputSink {
public:
    AudioOnlySink(const me::Timeline&        tl,
                  SinkCommon                 common,
                  std::vector<ClipTimeRange> ranges,
                  me::resource::CodecPool*   pool,
                  int64_t                    audio_bitrate)
        : tl_(tl),
          common_(std::move(common)),
          ranges_(std::move(ranges)),
          pool_(pool),
          audio_bitrate_(audio_bitrate) {}

    me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) override {
        if (demuxes.size() != ranges_.size()) {
            if (err) *err = "AudioOnlySink: demuxes / ranges size mismatch";
            return ME_E_INTERNAL;
        }
        if (tl_.tracks.empty() || tl_.clips.empty()) {
            if (err) *err = "AudioOnlySink: empty timeline";
            return ME_E_INVALID_ARG;
        }

        /* Build ReencodeOptions.segments from all audio clips — this
         * drives setup_h264_aac_encoder_mux's duration accounting.
         * (The segments are not drained as video; the mixer handles
         * actual audio content separately.) */
        ReencodeOptions opts;
        opts.out_path          = common_.out_path;
        opts.container         = common_.container;
        opts.video_codec       = "";                /* unused in audio-only */
        opts.audio_codec       = "aac";
        opts.audio_bitrate_bps = audio_bitrate_;
        opts.cancel            = common_.cancel;
        opts.on_ratio          = common_.on_ratio;
        opts.pool              = pool_;
        opts.target_color_space = common_.target_color_space;
        opts.ocio_config_path  = common_.ocio_config_path;
        opts.audio_only        = true;

        std::size_t first_audio_clip_idx = SIZE_MAX;
        for (std::size_t ci = 0; ci < tl_.clips.size(); ++ci) {
            const me::Clip& c = tl_.clips[ci];
            /* Walk timeline tracks to find this clip's track kind. */
            bool is_audio_clip = false;
            for (const auto& t : tl_.tracks) {
                if (t.id == c.track_id) {
                    is_audio_clip = (t.kind == me::TrackKind::Audio);
                    break;
                }
            }
            if (!is_audio_clip) continue;
            if (ci >= demuxes.size() || !demuxes[ci]) {
                if (err) *err = "AudioOnlySink: missing demux for audio clip";
                return ME_E_INVALID_ARG;
            }
            if (first_audio_clip_idx == SIZE_MAX) first_audio_clip_idx = ci;
            opts.segments.push_back(ReencodeSegment{
                demuxes[ci],
                ranges_[ci].source_start,
                ranges_[ci].source_duration,
                ranges_[ci].source_color_space,
            });
        }
        if (first_audio_clip_idx == SIZE_MAX) {
            if (err) *err = "AudioOnlySink: no audio clips found";
            return ME_E_INVALID_ARG;
        }

        /* Setup encoder + mux (audio-only mode). Sample demux is the
         * first audio clip's file — its audio stream params determine
         * the encoder's target (rate, fmt, ch_layout, frame_size). */
        std::unique_ptr<me::io::MuxContext> mux;
        me::resource::CodecPool::Ptr venc_owner, aenc_owner;
        detail::SharedEncState shared;
        if (auto s = setup_h264_aac_encoder_mux(
                opts, demuxes[first_audio_clip_idx]->fmt,
                mux, venc_owner, aenc_owner, shared, err);
            s != ME_OK) {
            return s;
        }
        if (!shared.aenc) {
            if (err) *err = "AudioOnlySink: audio encoder not created (setup failure)";
            return ME_E_INTERNAL;
        }
        struct FifoGuard {
            AVAudioFifo* f;
            ~FifoGuard() { if (f) av_audio_fifo_free(f); }
        } fifo_guard{shared.afifo};

        if (auto s = mux->open_avio(err);    s != ME_OK) return s;
        if (auto s = mux->write_header(err); s != ME_OK) return s;

        /* Build AudioMixer with encoder's params — same pattern as
         * ComposeSink's mixer wire-in. Target format is whatever
         * setup produced (AAC encoder uses FLTP natively). */
        me::audio::AudioMixerConfig mix_cfg;
        mix_cfg.target_rate = shared.aenc->sample_rate;
        mix_cfg.target_fmt  = shared.aenc->sample_fmt;
        if (av_channel_layout_copy(&mix_cfg.target_ch_layout,
                                    &shared.aenc->ch_layout) < 0) {
            if (err) *err = "AudioOnlySink: channel layout copy for mixer config";
            return ME_E_INTERNAL;
        }
        mix_cfg.frame_size = shared.aenc->frame_size > 0
            ? shared.aenc->frame_size : 1024;
        mix_cfg.peak_threshold = 0.95f;

        std::unique_ptr<me::audio::AudioMixer> mixer;
        const me_status_t build_s = me::audio::build_audio_mixer_for_timeline(
            tl_, *pool_, demuxes, mix_cfg, mixer, err);
        av_channel_layout_uninit(&mix_cfg.target_ch_layout);
        if (build_s != ME_OK) return build_s;

        /* Drain mixer → AAC encoder → mux. PTS stamped in encoder
         * time base (1/sample_rate), incremented by frame sample
         * count each iteration. Cancel-aware. */
        while (true) {
            if (shared.cancel &&
                shared.cancel->load(std::memory_order_acquire)) {
                return ME_E_CANCELLED;
            }
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
        /* Flush encoder with null frame. */
        if (auto s = detail::encode_audio_frame(
                nullptr, shared.aenc, shared.ofmt, shared.out_aidx, err);
            s != ME_OK) {
            return s;
        }

        if (auto s = mux->write_trailer(err); s != ME_OK) return s;
        if (shared.on_ratio) shared.on_ratio(1.0f);
        return ME_OK;
    }

private:
    const me::Timeline&        tl_;
    SinkCommon                 common_;
    std::vector<ClipTimeRange> ranges_;
    me::resource::CodecPool*   pool_ = nullptr;
    int64_t                    audio_bitrate_ = 0;
};

}  // namespace

std::unique_ptr<OutputSink> make_audio_only_sink(
    const me::Timeline&            tl,
    const me_output_spec_t&        spec,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    std::string*                   err) {

    const CodecSelection sel = resolve_codec_selection(spec);
    if (sel.audio_codec != ME_AUDIO_CODEC_AAC) {
        if (err) *err = "audio-only path requires audio_codec=aac; other codecs unsupported";
        return nullptr;
    }
    if (!pool) {
        if (err) *err = "audio-only path requires a CodecPool (engine->codecs)";
        return nullptr;
    }
    if (clip_ranges.empty()) {
        if (err) *err = "audio-only path requires at least one clip";
        return nullptr;
    }
    /* At least one audio track required; no video tracks allowed
     * (callers with video use make_compose_sink or make_output_sink). */
    bool any_audio = false;
    bool any_video = false;
    for (const auto& t : tl.tracks) {
        if (t.kind == me::TrackKind::Audio) any_audio = true;
        if (t.kind == me::TrackKind::Video) any_video = true;
    }
    if (!any_audio) {
        if (err) *err = "make_audio_only_sink: timeline has no audio tracks";
        return nullptr;
    }
    if (any_video) {
        if (err) *err = "make_audio_only_sink: timeline has video tracks — use make_compose_sink";
        return nullptr;
    }

    return std::make_unique<AudioOnlySink>(
        tl,
        std::move(common),
        std::move(clip_ranges),
        pool,
        spec.audio_bitrate_bps);
}

}  // namespace me::orchestrator
