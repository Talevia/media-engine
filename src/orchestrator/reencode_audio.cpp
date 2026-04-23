#include "orchestrator/reencode_audio.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <string>

namespace me::orchestrator::detail {

namespace {

std::string av_err_str(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

using PacketPtr   = me::io::AvPacketPtr;

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

}  // namespace me::orchestrator::detail
