#include "orchestrator/reencode_video.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <string>

namespace me::orchestrator::detail {

namespace {

std::string av_err_str(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

using CodecCtxPtr = me::io::AvCodecContextPtr;
using PacketPtr   = me::io::AvPacketPtr;

}  // namespace

me_status_t open_video_encoder(const AVCodecContext* dec,
                               AVRational            stream_time_base,
                               int64_t               bitrate_bps,
                               bool                  global_header,
                               CodecCtxPtr&          out_enc,
                               AVPixelFormat&        out_target_pix,
                               std::string*          err) {
    const AVCodec* enc = avcodec_find_encoder_by_name("h264_videotoolbox");
    if (!enc) {
        if (err) *err = "encoder h264_videotoolbox not available in this FFmpeg build";
        return ME_E_UNSUPPORTED;
    }
    CodecCtxPtr ctx(avcodec_alloc_context3(enc));
    if (!ctx) return ME_E_OUT_OF_MEMORY;

    ctx->width               = dec->width;
    ctx->height              = dec->height;
    ctx->pix_fmt             = AV_PIX_FMT_NV12;
    ctx->time_base           = stream_time_base;
    ctx->framerate           = dec->framerate;
    ctx->sample_aspect_ratio = dec->sample_aspect_ratio;
    ctx->color_range         = dec->color_range;
    ctx->color_primaries     = dec->color_primaries;
    ctx->color_trc           = dec->color_trc;
    ctx->colorspace          = dec->colorspace;
    ctx->bit_rate            = (bitrate_bps > 0) ? bitrate_bps : 6'000'000;
    /* MP4 / MOV need extradata carried in the container's 'avcC' box, not
     * prefixed to keyframes — MUST be set before avcodec_open2. */
    if (global_header) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int rc = avcodec_open2(ctx.get(), enc, nullptr);
    if (rc < 0) {
        if (err) *err = "open h264_videotoolbox: " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    out_enc = std::move(ctx);
    out_target_pix = AV_PIX_FMT_NV12;
    return ME_OK;
}

me_status_t encode_video_frame(AVFrame*         in_frame,
                               AVCodecContext*  enc,
                               SwsContext*      sws,
                               AVFrame*         scratch_nv12,
                               AVFormatContext* ofmt,
                               int              out_stream_idx,
                               AVRational       in_stream_tb,
                               std::string*     err) {
    AVFrame* to_encode = in_frame;

    if (in_frame && sws) {
        int rc = sws_scale(sws,
                           in_frame->data, in_frame->linesize,
                           0, in_frame->height,
                           scratch_nv12->data, scratch_nv12->linesize);
        if (rc < 0) {
            if (err) *err = "sws_scale: " + av_err_str(rc);
            return ME_E_INTERNAL;
        }
        scratch_nv12->pts     = av_rescale_q(in_frame->pts,     in_stream_tb, enc->time_base);
        scratch_nv12->pkt_dts = av_rescale_q(in_frame->pkt_dts, in_stream_tb, enc->time_base);
        to_encode = scratch_nv12;
    } else if (in_frame) {
        in_frame->pts = av_rescale_q(in_frame->pts, in_stream_tb, enc->time_base);
    }

    int rc = avcodec_send_frame(enc, to_encode);
    if (rc < 0 && rc != AVERROR_EOF) {
        if (err) *err = "send_frame(video): " + av_err_str(rc);
        return ME_E_ENCODE;
    }

    PacketPtr out_pkt(av_packet_alloc());
    while (true) {
        rc = avcodec_receive_packet(enc, out_pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            if (err) *err = "receive_packet(video): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
        out_pkt->stream_index = out_stream_idx;
        av_packet_rescale_ts(out_pkt.get(), enc->time_base,
                              ofmt->streams[out_stream_idx]->time_base);
        rc = av_interleaved_write_frame(ofmt, out_pkt.get());
        av_packet_unref(out_pkt.get());
        if (rc < 0) {
            if (err) *err = "write_frame(video): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
    }
    return ME_OK;
}

}  // namespace me::orchestrator::detail
