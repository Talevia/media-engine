#include "orchestrator/reencode_video.hpp"

#include "io/av_err.hpp"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <string>

namespace me::orchestrator::detail {

namespace {

using me::io::av_err_str;
using PacketPtr = me::io::AvPacketPtr;

}  // namespace

me_status_t open_video_encoder(me::resource::CodecPool&      pool,
                               const AVCodecContext*         dec,
                               AVRational                    stream_time_base,
                               int64_t                       bitrate_bps,
                               bool                          global_header,
                               const std::string&            video_codec,
                               me::resource::CodecPool::Ptr& out_enc,
                               AVPixelFormat&                out_target_pix,
                               std::string*                  err) {
    /* Dispatch table — kept tight so the hot path stays branch-free
     * after first-call dispatch. The two tuples below are ABI-locked:
     * adding a new HW path means a new tuple, never editing the
     * existing ones (callers depend on the documented pix_fmt
     * contract). */
    const bool is_hevc = (video_codec == "hevc");
    const char* const  enc_name = is_hevc ? "hevc_videotoolbox"
                                          : "h264_videotoolbox";
    const AVPixelFormat enc_pix  = is_hevc ? AV_PIX_FMT_P010LE
                                           : AV_PIX_FMT_NV12;
    /* HEVC default bitrate is higher than h264 because Main10 sources
     * carry more bits per pixel (10 vs 8) and HDR10 content needs
     * higher fidelity to avoid banding on bright gradients. 12 Mbps
     * matches the recommended floor for 1080p30 HDR10 in the
     * Apple ProRes / VideoToolbox bitrate tables. */
    const int64_t default_bitrate = is_hevc ? 12'000'000 : 6'000'000;

    if (!video_codec.empty() && video_codec != "h264" && !is_hevc) {
        if (err) *err = "open_video_encoder: unsupported video_codec '" +
                        video_codec + "' (expected '' / 'h264' / 'hevc')";
        /* LEGIT: codec-name dispatch is closed over the set we ship.
         * New codec names land via additional `is_<codec>` branches
         * above; until then UNSUPPORTED is the right shape. */
        return ME_E_UNSUPPORTED;
    }

    const AVCodec* enc = avcodec_find_encoder_by_name(enc_name);
    if (!enc) {
        if (err) *err = std::string{"encoder "} + enc_name +
                        " not available in this FFmpeg build";
        /* LEGIT: Linux / Windows hosts lack videotoolbox; reject with
         * a clear diagnostic so callers know the platform gate. */
        return ME_E_UNSUPPORTED;
    }
    auto ctx = pool.allocate(enc);
    if (!ctx) return ME_E_OUT_OF_MEMORY;

    ctx->width               = dec->width;
    ctx->height              = dec->height;
    ctx->pix_fmt             = enc_pix;
    ctx->time_base           = stream_time_base;
    ctx->framerate           = dec->framerate;
    ctx->sample_aspect_ratio = dec->sample_aspect_ratio;
    /* Color tags propagate verbatim from the decoder. For HDR sources
     * tagged BT.2020 + PQ the resulting `color_primaries` /
     * `color_trc` / `colorspace` cause `hevc_videotoolbox` to emit ST
     * 2086 mastering-display + CTA-861.3 content-light SEI from the
     * codec's defaults. Custom mastering-display values (host-supplied
     * MaxCLL / MaxFALL / chromaticities) flow via packet side-data in
     * a future cycle (`test-hdr-metadata-propagate`). */
    ctx->color_range         = dec->color_range;
    ctx->color_primaries     = dec->color_primaries;
    ctx->color_trc           = dec->color_trc;
    ctx->colorspace          = dec->colorspace;
    ctx->bit_rate            = (bitrate_bps > 0) ? bitrate_bps
                                                  : default_bitrate;
    /* MP4 / MOV need extradata carried in the container's 'avcC' / 'hvcC'
     * box, not prefixed to keyframes — MUST be set before avcodec_open2. */
    if (global_header) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* Hint to the encoder / its helpers to avoid baking libav version
     * strings into extradata or packets. Paired with AVFMT_FLAG_BITEXACT
     * on the muxer — see test_determinism / decision
     * 2026-04-23-debt-render-bitexact-flags. videotoolbox is HW so
     * this flag is advisory; it's still worth passing for the software-
     * path intent and for other encoders future re-encode paths may
     * pick up. */
    ctx->flags |= AV_CODEC_FLAG_BITEXACT;

    int rc = avcodec_open2(ctx.get(), enc, nullptr);
    if (rc < 0) {
        if (err) *err = std::string{"open "} + enc_name + ": " + av_err_str(rc);
        return ME_E_ENCODE;
    }
    out_enc = std::move(ctx);
    out_target_pix = enc_pix;
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

int64_t remap_source_pts_to_output(int64_t     src_pts,
                                    int64_t     first_src_pts,
                                    AVRational  src_tb,
                                    AVRational  out_tb,
                                    int64_t     segment_base_out_pts) {
    const int64_t src_offset     = src_pts - first_src_pts;
    const int64_t out_offset     = av_rescale_q(src_offset, src_tb, out_tb);
    return segment_base_out_pts + out_offset;
}

}  // namespace me::orchestrator::detail
