#include "orchestrator/reencode_audio.hpp"

#include "io/av_err.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <string>

namespace me::orchestrator::detail {

namespace {

using me::io::av_err_str;
using FramePtr  = me::io::AvFramePtr;
using PacketPtr = me::io::AvPacketPtr;

}  // namespace

me_status_t open_audio_encoder(me::resource::CodecPool&      pool,
                               const AVCodecContext*         dec,
                               int64_t                       bitrate_bps,
                               bool                          global_header,
                               me::resource::CodecPool::Ptr& out_enc,
                               std::string*                  err) {
    const AVCodec* enc = avcodec_find_encoder_by_name("aac");
    if (!enc) {
        if (err) *err = "encoder aac not available";
        return ME_E_UNSUPPORTED;
    }
    auto ctx = pool.allocate(enc);
    if (!ctx) return ME_E_OUT_OF_MEMORY;

    /* FFmpeg's built-in AAC encoder supports a fixed sample rate set
     * (MPEG-4 AAC table). Clamp off-grid input to 48 kHz; native
     * avcodec_get_supported_config could be used instead but adds API
     * version surface without benefit — these rates don't change. */
    static const int aac_rates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000,
        44100, 48000, 64000, 88200, 96000, 0
    };
    int sample_rate = 48000;
    for (int i = 0; aac_rates[i]; ++i) {
        if (aac_rates[i] == dec->sample_rate) { sample_rate = dec->sample_rate; break; }
    }
    ctx->sample_rate = sample_rate;
    ctx->bit_rate    = (bitrate_bps > 0) ? bitrate_bps : 128'000;

    /* Built-in AAC encoder only accepts planar float samples. */
    ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    ctx->time_base   = AVRational{1, sample_rate};

    /* Channel layout: inherit when set; otherwise default by nb_channels. */
    if (dec->ch_layout.nb_channels > 0 && dec->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_copy(&ctx->ch_layout, &dec->ch_layout);
    } else {
        av_channel_layout_default(&ctx->ch_layout,
                                   dec->ch_layout.nb_channels > 0
                                       ? dec->ch_layout.nb_channels : 2);
    }
    if (global_header) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* See reencode_video's matching comment: paired with AVFMT_FLAG_BITEXACT
     * on the muxer to make the software reencode path byte-deterministic
     * across libav versions. */
    ctx->flags |= AV_CODEC_FLAG_BITEXACT;

    int rc = avcodec_open2(ctx.get(), enc, nullptr);
    if (rc < 0) {
        if (err) *err = "open aac: " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    out_enc = std::move(ctx);
    return ME_OK;
}

me_status_t encode_audio_frame(AVFrame*         in_frame,
                               AVCodecContext*  enc,
                               AVFormatContext* ofmt,
                               int              out_stream_idx,
                               std::string*     err) {
    int rc = avcodec_send_frame(enc, in_frame);
    if (rc < 0 && rc != AVERROR_EOF) {
        if (err) *err = "send_frame(audio): " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    PacketPtr out_pkt(av_packet_alloc());
    while (true) {
        rc = avcodec_receive_packet(enc, out_pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            if (err) *err = "receive_packet(audio): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
        out_pkt->stream_index = out_stream_idx;
        av_packet_rescale_ts(out_pkt.get(), enc->time_base,
                              ofmt->streams[out_stream_idx]->time_base);
        rc = av_interleaved_write_frame(ofmt, out_pkt.get());
        av_packet_unref(out_pkt.get());
        if (rc < 0) {
            if (err) *err = "write_frame(audio): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
    }
    return ME_OK;
}

me_status_t drain_audio_fifo(AVAudioFifo*     afifo,
                             AVCodecContext*  aenc,
                             AVFormatContext* ofmt,
                             int              out_stream_idx,
                             int64_t*         next_pts_in_enc_tb,
                             bool             flush,
                             std::string*     err) {
    if (!afifo || !aenc || !ofmt || !next_pts_in_enc_tb) {
        if (err) *err = "drain_audio_fifo: null arg";
        return ME_E_INVALID_ARG;
    }
    while (true) {
        const int have = av_audio_fifo_size(afifo);
        const int need = aenc->frame_size;
        if (!flush && have < need) return ME_OK;
        if (flush && have == 0)    return ME_OK;

        const int this_frame = flush ? std::min(have, need ? need : have) : need;
        FramePtr out_af(av_frame_alloc());
        out_af->nb_samples = this_frame;
        out_af->format     = aenc->sample_fmt;
        av_channel_layout_copy(&out_af->ch_layout, &aenc->ch_layout);
        out_af->sample_rate = aenc->sample_rate;
        int r = av_frame_get_buffer(out_af.get(), 0);
        if (r < 0) {
            if (err) *err = "frame_get_buffer(audio): " + av_err_str(r);
            return ME_E_OUT_OF_MEMORY;
        }
        r = av_audio_fifo_read(afifo, (void**)out_af->data, this_frame);
        if (r < 0) {
            if (err) *err = "audio_fifo_read: " + av_err_str(r);
            return ME_E_INTERNAL;
        }
        out_af->pts = *next_pts_in_enc_tb;
        *next_pts_in_enc_tb += this_frame;

        me_status_t st = encode_audio_frame(out_af.get(), aenc, ofmt, out_stream_idx, err);
        if (st != ME_OK) return st;
    }
}

me_status_t feed_audio_frame(AVFrame*         in_frame,
                             SwrContext*      swr,
                             int              src_sample_rate,
                             AVAudioFifo*     afifo,
                             AVCodecContext*  aenc,
                             AVFormatContext* ofmt,
                             int              out_stream_idx,
                             int64_t*         next_pts_in_enc_tb,
                             std::string*     err) {
    if (!swr || !afifo || !aenc || !ofmt || !next_pts_in_enc_tb) {
        if (err) *err = "feed_audio_frame: null arg";
        return ME_E_INVALID_ARG;
    }

    /* Swr flush path: in_frame == nullptr, pull residual samples out of
     * swr into the FIFO. The FIFO is NOT force-drained here — that's the
     * end-of-stream drain_audio_fifo(..., flush=true) job after all
     * segments' residuals are collected. */
    if (!in_frame) {
        const int out_samples_est =
            static_cast<int>(av_rescale_rnd(swr_get_delay(swr, src_sample_rate),
                                             aenc->sample_rate, src_sample_rate,
                                             AV_ROUND_UP));
        if (out_samples_est <= 0) return ME_OK;
        uint8_t** out_data = nullptr;
        int out_linesize = 0;
        int r = av_samples_alloc_array_and_samples(&out_data, &out_linesize,
                                                    aenc->ch_layout.nb_channels,
                                                    out_samples_est, aenc->sample_fmt, 0);
        if (r < 0) {
            if (err) *err = "samples_alloc(flush): " + av_err_str(r);
            return ME_E_OUT_OF_MEMORY;
        }
        int converted = swr_convert(swr, out_data, out_samples_est, nullptr, 0);
        if (converted > 0) {
            int fr = av_audio_fifo_realloc(afifo, av_audio_fifo_size(afifo) + converted);
            if (fr >= 0) av_audio_fifo_write(afifo, (void**)out_data, converted);
        }
        if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
        return ME_OK;
    }

    /* Normal path: convert one decoded frame's worth of samples, push
     * into the FIFO, then drain the FIFO in encoder-sized chunks. */
    const int in_samples = in_frame->nb_samples;
    const int out_samples_est =
        static_cast<int>(av_rescale_rnd(swr_get_delay(swr, src_sample_rate) + in_samples,
                                         aenc->sample_rate, src_sample_rate, AV_ROUND_UP));
    uint8_t** out_data = nullptr;
    int out_linesize = 0;
    int r = av_samples_alloc_array_and_samples(&out_data, &out_linesize,
                                                aenc->ch_layout.nb_channels,
                                                out_samples_est, aenc->sample_fmt, 0);
    if (r < 0) {
        if (err) *err = "samples_alloc: " + av_err_str(r);
        return ME_E_OUT_OF_MEMORY;
    }
    int converted = swr_convert(swr, out_data, out_samples_est,
                                (const uint8_t**)in_frame->data, in_samples);
    if (converted < 0) {
        if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
        if (err) *err = "swr_convert: " + av_err_str(converted);
        return ME_E_INTERNAL;
    }
    r = av_audio_fifo_realloc(afifo, av_audio_fifo_size(afifo) + converted);
    if (r < 0) {
        if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
        if (err) *err = "fifo_realloc: " + av_err_str(r);
        return ME_E_OUT_OF_MEMORY;
    }
    r = av_audio_fifo_write(afifo, (void**)out_data, converted);
    if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
    if (r < 0) {
        if (err) *err = "fifo_write: " + av_err_str(r);
        return ME_E_INTERNAL;
    }

    return drain_audio_fifo(afifo, aenc, ofmt, out_stream_idx,
                             next_pts_in_enc_tb, /*flush=*/false, err);
}

}  // namespace me::orchestrator::detail
