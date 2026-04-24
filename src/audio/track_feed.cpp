#include "audio/track_feed.hpp"

#include "audio/resample.hpp"
#include "io/demux_context.hpp"
#include "orchestrator/frame_puller.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

namespace me::audio {

AudioTrackFeed::~AudioTrackFeed() {
    av_channel_layout_uninit(&target_ch_layout);
}

AudioTrackFeed::AudioTrackFeed(AudioTrackFeed&& other) noexcept
    : demux(std::move(other.demux)),
      audio_stream_idx(other.audio_stream_idx),
      dec(std::move(other.dec)),
      pkt_scratch(std::move(other.pkt_scratch)),
      frame_scratch(std::move(other.frame_scratch)),
      target_rate(other.target_rate),
      target_fmt(other.target_fmt),
      gain_linear(other.gain_linear),
      eof(other.eof) {
    target_ch_layout = AVChannelLayout{};
    av_channel_layout_copy(&target_ch_layout, &other.target_ch_layout);
    av_channel_layout_uninit(&other.target_ch_layout);
    other.audio_stream_idx = -1;
    other.eof              = true;
}

AudioTrackFeed& AudioTrackFeed::operator=(AudioTrackFeed&& other) noexcept {
    if (this != &other) {
        av_channel_layout_uninit(&target_ch_layout);
        demux            = std::move(other.demux);
        audio_stream_idx = other.audio_stream_idx;
        dec              = std::move(other.dec);
        pkt_scratch      = std::move(other.pkt_scratch);
        frame_scratch    = std::move(other.frame_scratch);
        target_rate      = other.target_rate;
        target_fmt       = other.target_fmt;
        av_channel_layout_copy(&target_ch_layout, &other.target_ch_layout);
        av_channel_layout_uninit(&other.target_ch_layout);
        gain_linear      = other.gain_linear;
        eof              = other.eof;
        other.audio_stream_idx = -1;
        other.eof              = true;
    }
    return *this;
}

me_status_t open_audio_track_feed(
    std::shared_ptr<me::io::DemuxContext> demux,
    me::resource::CodecPool&               pool,
    int                                    target_rate,
    AVSampleFormat                         target_fmt,
    const AVChannelLayout&                 target_ch_layout,
    float                                  gain_linear,
    AudioTrackFeed&                        out,
    std::string*                           err) {

    if (!demux) {
        if (err) *err = "open_audio_track_feed: null demux";
        return ME_E_INVALID_ARG;
    }
    AVFormatContext* fmt = demux->fmt;
    if (!fmt) {
        if (err) *err = "open_audio_track_feed: demux has null AVFormatContext";
        return ME_E_INVALID_ARG;
    }
    if (target_rate <= 0 || target_fmt == AV_SAMPLE_FMT_NONE) {
        if (err) *err = "open_audio_track_feed: target_rate must be > 0 and target_fmt != NONE";
        return ME_E_INVALID_ARG;
    }

    const int asi =
        av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (asi < 0) {
        if (err) *err = "open_audio_track_feed: no audio stream in demux";
        return ME_E_NOT_FOUND;
    }
    AVStream* st = fmt->streams[asi];

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        if (err) *err = "open_audio_track_feed: no decoder for audio codec_id";
        return ME_E_UNSUPPORTED;
    }

    auto dec = pool.allocate(codec);
    if (!dec) {
        if (err) *err = "open_audio_track_feed: avcodec_alloc_context3 failed";
        return ME_E_OUT_OF_MEMORY;
    }
    if (avcodec_parameters_to_context(dec.get(), st->codecpar) < 0) {
        if (err) *err = "open_audio_track_feed: avcodec_parameters_to_context failed";
        return ME_E_INTERNAL;
    }
    if (avcodec_open2(dec.get(), codec, nullptr) < 0) {
        if (err) *err = "open_audio_track_feed: avcodec_open2 failed";
        return ME_E_INTERNAL;
    }

    me::io::AvPacketPtr pkt{av_packet_alloc()};
    me::io::AvFramePtr  frm{av_frame_alloc()};
    if (!pkt || !frm) {
        if (err) *err = "open_audio_track_feed: av_packet_alloc / av_frame_alloc failed";
        return ME_E_OUT_OF_MEMORY;
    }

    /* Uninit any prior layout the caller may have left in out, then copy. */
    av_channel_layout_uninit(&out.target_ch_layout);
    if (av_channel_layout_copy(&out.target_ch_layout, &target_ch_layout) < 0) {
        if (err) *err = "open_audio_track_feed: av_channel_layout_copy failed";
        return ME_E_INTERNAL;
    }

    out.demux            = std::move(demux);
    out.audio_stream_idx = asi;
    out.dec              = std::move(dec);
    out.pkt_scratch      = std::move(pkt);
    out.frame_scratch    = std::move(frm);
    out.target_rate      = target_rate;
    out.target_fmt       = target_fmt;
    out.gain_linear      = gain_linear;
    out.eof              = false;
    return ME_OK;
}

namespace {

/* In-place linear gain on a planar-float AVFrame. Silent input
 * stays silent regardless of gain; for FLTP the gain applies per
 * channel plane, per sample. gain == 1.0f is a no-op short-circuit
 * (common case for first pass / unity-gain tracks). */
void apply_gain_fltp(AVFrame* f, float gain) {
    if (gain == 1.0f) return;
    const int planes   = f->ch_layout.nb_channels;
    const int nb_samps = f->nb_samples;
    for (int ch = 0; ch < planes; ++ch) {
        auto* p = reinterpret_cast<float*>(f->extended_data[ch]);
        for (int i = 0; i < nb_samps; ++i) p[i] *= gain;
    }
}

}  // namespace

me_status_t pull_next_processed_audio_frame(
    AudioTrackFeed& feed,
    AVFrame**       out_frame,
    std::string*    err) {

    if (!out_frame) {
        if (err) *err = "pull_next_processed_audio_frame: null out_frame";
        return ME_E_INVALID_ARG;
    }
    *out_frame = nullptr;
    if (feed.eof) return ME_E_NOT_FOUND;
    if (!feed.demux || !feed.demux->fmt || !feed.dec ||
        !feed.pkt_scratch || !feed.frame_scratch) {
        if (err) *err = "pull_next_processed_audio_frame: feed not opened";
        return ME_E_INVALID_ARG;
    }

    const me_status_t s = me::orchestrator::pull_next_audio_frame(
        feed.demux->fmt, feed.audio_stream_idx, feed.dec.get(),
        feed.pkt_scratch.get(), feed.frame_scratch.get(), err);
    if (s == ME_E_NOT_FOUND) {
        feed.eof = true;
        return ME_E_NOT_FOUND;
    }
    if (s != ME_OK) return s;

    AVFrame* resampled = nullptr;
    const me_status_t rs = resample_to(feed.frame_scratch.get(),
                                        feed.target_rate,
                                        feed.target_fmt,
                                        feed.target_ch_layout,
                                        &resampled, err);
    av_frame_unref(feed.frame_scratch.get());
    if (rs != ME_OK) return rs;

    if (feed.target_fmt == AV_SAMPLE_FMT_FLTP) {
        apply_gain_fltp(resampled, feed.gain_linear);
    }

    *out_frame = resampled;
    return ME_OK;
}

}  // namespace me::audio
