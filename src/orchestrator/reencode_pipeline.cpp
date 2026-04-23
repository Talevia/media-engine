#include "orchestrator/reencode_pipeline.hpp"

#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/audio_fifo.h>
}

#include <memory>
#include <string>
#include <vector>

namespace me::orchestrator {

namespace {

std::string av_err_str(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

/* Short file-local aliases over the shared me::io deleters — keeps the
 * dense decode/encode code below readable without repeating the full
 * namespace-qualified name every few lines. AVFormatContext output stays
 * manually managed because its lifetime is entangled with avio_open/close
 * (see io-mux-context-raii backlog item). */
using CodecCtxPtr = me::io::AvCodecContextPtr;
using FramePtr    = me::io::AvFramePtr;
using PacketPtr   = me::io::AvPacketPtr;
using SwsPtr      = me::io::SwsContextPtr;
using SwrPtr      = me::io::SwrContextPtr;

/* Pick the best video/audio stream index per libavformat conventions. */
int best_stream(AVFormatContext* fmt, AVMediaType type) {
    return av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
}

/* Video encoder bootstrap. Opens h264_videotoolbox sized / framed to match
 * the decoded stream. NV12 is the native VideoToolbox surface format; if the
 * decoder yields something else we stage an sws_scale into NV12. */
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

    ctx->width      = dec->width;
    ctx->height     = dec->height;
    ctx->pix_fmt    = AV_PIX_FMT_NV12;
    ctx->time_base  = stream_time_base;            /* same tb as input stream */
    ctx->framerate  = dec->framerate;
    ctx->sample_aspect_ratio = dec->sample_aspect_ratio;
    ctx->color_range    = dec->color_range;
    ctx->color_primaries = dec->color_primaries;
    ctx->color_trc      = dec->color_trc;
    ctx->colorspace     = dec->colorspace;
    ctx->bit_rate       = (bitrate_bps > 0) ? bitrate_bps : 6'000'000;
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

/* Audio encoder bootstrap. libavcodec's built-in AAC (not aac_at) stays in
 * pure LGPL. Sample rate and channel layout track the decoded stream when
 * possible; AAC only supports certain rates, so we fall back to 48000 if
 * the source is off-grid. */
me_status_t open_audio_encoder(const AVCodecContext* dec,
                               int64_t               bitrate_bps,
                               bool                  global_header,
                               CodecCtxPtr&          out_enc,
                               std::string*          err) {
    const AVCodec* enc = avcodec_find_encoder_by_name("aac");
    if (!enc) {
        if (err) *err = "encoder aac not available";
        return ME_E_UNSUPPORTED;
    }
    CodecCtxPtr ctx(avcodec_alloc_context3(enc));
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
        av_channel_layout_default(&ctx->ch_layout, dec->ch_layout.nb_channels > 0
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

me_status_t open_decoder(AVStream* in_stream, CodecCtxPtr& out, std::string* err) {
    const AVCodec* dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!dec) {
        if (err) *err = std::string("no decoder for ") + avcodec_get_name(in_stream->codecpar->codec_id);
        return ME_E_UNSUPPORTED;
    }
    CodecCtxPtr ctx(avcodec_alloc_context3(dec));
    if (!ctx) return ME_E_OUT_OF_MEMORY;

    int rc = avcodec_parameters_to_context(ctx.get(), in_stream->codecpar);
    if (rc < 0) {
        if (err) *err = "parameters_to_context: " + av_err_str(rc);
        return ME_E_INTERNAL;
    }
    ctx->pkt_timebase = in_stream->time_base;
    rc = avcodec_open2(ctx.get(), dec, nullptr);
    if (rc < 0) {
        if (err) *err = std::string("open decoder ") + dec->name + ": " + av_err_str(rc);
        return ME_E_DECODE;
    }
    out = std::move(ctx);
    return ME_OK;
}

/* Encode one decoded video frame (or nullptr for flush). Scales into NV12
 * when needed. The FIFO of encoded packets is drained by avcodec_receive_packet
 * and written to the output mux. */
me_status_t encode_video_frame(AVFrame*           in_frame,       /* may be nullptr for flush */
                               AVCodecContext*    enc,
                               SwsContext*        sws,            /* may be nullptr */
                               AVFrame*           scratch_nv12,   /* may be nullptr if sws nullptr */
                               AVFormatContext*   ofmt,
                               int                out_stream_idx,
                               AVRational         in_stream_tb,
                               std::string*       err) {
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
        scratch_nv12->pts        = av_rescale_q(in_frame->pts, in_stream_tb, enc->time_base);
        scratch_nv12->pkt_dts    = av_rescale_q(in_frame->pkt_dts, in_stream_tb, enc->time_base);
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
        av_packet_rescale_ts(out_pkt.get(), enc->time_base, ofmt->streams[out_stream_idx]->time_base);
        rc = av_interleaved_write_frame(ofmt, out_pkt.get());
        av_packet_unref(out_pkt.get());
        if (rc < 0) {
            if (err) *err = "write_frame(video): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
    }
    return ME_OK;
}

me_status_t encode_audio_frame(AVFrame*           in_frame,         /* may be nullptr */
                               AVCodecContext*    enc,
                               AVFormatContext*   ofmt,
                               int                out_stream_idx,
                               std::string*       err) {
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
        av_packet_rescale_ts(out_pkt.get(), enc->time_base, ofmt->streams[out_stream_idx]->time_base);
        rc = av_interleaved_write_frame(ofmt, out_pkt.get());
        av_packet_unref(out_pkt.get());
        if (rc < 0) {
            if (err) *err = "write_frame(audio): " + av_err_str(rc);
            return ME_E_ENCODE;
        }
    }
    return ME_OK;
}

}  // namespace

me_status_t reencode_mux(io::DemuxContext&            demux,
                         const ReencodeOptions&       opts,
                         std::string*                 err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = std::move(msg);
        return s;
    };

    AVFormatContext* ifmt = demux.fmt;
    if (!ifmt) return fail(ME_E_INVALID_ARG, "demux context has no AVFormatContext");

    if (opts.video_codec != "h264") {
        return fail(ME_E_UNSUPPORTED,
                    "video_codec=\"" + opts.video_codec + "\" not supported (expected \"h264\")");
    }
    if (opts.audio_codec != "aac") {
        return fail(ME_E_UNSUPPORTED,
                    "audio_codec=\"" + opts.audio_codec + "\" not supported (expected \"aac\")");
    }

    const int vsi = best_stream(ifmt, AVMEDIA_TYPE_VIDEO);
    const int asi = best_stream(ifmt, AVMEDIA_TYPE_AUDIO);
    if (vsi < 0 && asi < 0) {
        return fail(ME_E_INVALID_ARG, "input has neither video nor audio");
    }

    /* --- Decoders --------------------------------------------------------- */
    CodecCtxPtr vdec, adec;
    if (vsi >= 0) {
        me_status_t s = open_decoder(ifmt->streams[vsi], vdec, err);
        if (s != ME_OK) return s;
    }
    if (asi >= 0) {
        me_status_t s = open_decoder(ifmt->streams[asi], adec, err);
        if (s != ME_OK) return s;
    }

    /* --- Output container + streams -------------------------------------- */
    AVFormatContext* ofmt = nullptr;
    const char* format_name = opts.container.empty() ? nullptr : opts.container.c_str();
    int rc = avformat_alloc_output_context2(&ofmt, nullptr, format_name, opts.out_path.c_str());
    if (rc < 0 || !ofmt) return fail(ME_E_INTERNAL, "alloc output: " + av_err_str(rc));

    auto cleanup = [&]() {
        if (ofmt) {
            if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb) avio_closep(&ofmt->pb);
            avformat_free_context(ofmt);
            ofmt = nullptr;
        }
    };

    int out_vidx = -1, out_aidx = -1;
    CodecCtxPtr venc, aenc;
    AVPixelFormat venc_target_pix = AV_PIX_FMT_NONE;

    if (vdec) {
        /* Output video stream: use same time_base as input stream so PTS maps 1:1. */
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) { cleanup(); return fail(ME_E_OUT_OF_MEMORY, "new_stream(video)"); }
        out_s->time_base = ifmt->streams[vsi]->time_base;

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = open_video_encoder(vdec.get(), ifmt->streams[vsi]->time_base,
                                           opts.video_bitrate_bps, global_header,
                                           venc, venc_target_pix, err);
        if (s != ME_OK) { cleanup(); return s; }

        rc = avcodec_parameters_from_context(out_s->codecpar, venc.get());
        if (rc < 0) { cleanup(); return fail(ME_E_INTERNAL, "params_from_context(video): " + av_err_str(rc)); }
        out_s->avg_frame_rate = vdec->framerate;
        out_s->r_frame_rate   = vdec->framerate;
        out_vidx = out_s->index;
    }
    if (adec) {
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) { cleanup(); return fail(ME_E_OUT_OF_MEMORY, "new_stream(audio)"); }

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = open_audio_encoder(adec.get(), opts.audio_bitrate_bps,
                                           global_header, aenc, err);
        if (s != ME_OK) { cleanup(); return s; }
        out_s->time_base = aenc->time_base;

        rc = avcodec_parameters_from_context(out_s->codecpar, aenc.get());
        if (rc < 0) { cleanup(); return fail(ME_E_INTERNAL, "params_from_context(audio): " + av_err_str(rc)); }
        out_aidx = out_s->index;
    }

    /* --- Open output file, write header ---------------------------------- */
    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&ofmt->pb, opts.out_path.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) { cleanup(); return fail(ME_E_IO, "avio_open: " + av_err_str(rc)); }
    }
    rc = avformat_write_header(ofmt, nullptr);
    if (rc < 0) { cleanup(); return fail(ME_E_ENCODE, "write_header: " + av_err_str(rc)); }

    /* --- Scratch: sws / swr / frames -------------------------------------- */
    SwsPtr sws;
    FramePtr v_scratch;      /* NV12 staging frame */
    if (vdec) {
        const AVPixelFormat src_pix = (AVPixelFormat)vdec->pix_fmt;
        if (src_pix != venc_target_pix) {
            sws.reset(sws_getContext(vdec->width, vdec->height, src_pix,
                                     venc->width, venc->height, venc_target_pix,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!sws) { cleanup(); return fail(ME_E_INTERNAL, "sws_getContext"); }
            v_scratch.reset(av_frame_alloc());
            v_scratch->format = venc_target_pix;
            v_scratch->width  = venc->width;
            v_scratch->height = venc->height;
            rc = av_frame_get_buffer(v_scratch.get(), 32);
            if (rc < 0) { cleanup(); return fail(ME_E_OUT_OF_MEMORY, "frame_get_buffer(video): " + av_err_str(rc)); }
        }
    }

    SwrPtr swr;
    AVAudioFifo* afifo = nullptr;
    if (adec) {
        SwrContext* raw_swr = nullptr;
        rc = swr_alloc_set_opts2(&raw_swr,
                                 &aenc->ch_layout, aenc->sample_fmt, aenc->sample_rate,
                                 &adec->ch_layout, adec->sample_fmt, adec->sample_rate,
                                 0, nullptr);
        if (rc < 0 || !raw_swr) { cleanup(); return fail(ME_E_INTERNAL, "swr_alloc: " + av_err_str(rc)); }
        swr.reset(raw_swr);
        rc = swr_init(swr.get());
        if (rc < 0) { cleanup(); return fail(ME_E_INTERNAL, "swr_init: " + av_err_str(rc)); }

        /* AAC encoder wants a fixed frame_size per call. We buffer resampled
         * samples in an FIFO and flush frame_size chunks to the encoder. */
        afifo = av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, 1);
        if (!afifo) { cleanup(); return fail(ME_E_OUT_OF_MEMORY, "audio_fifo_alloc"); }
    }

    /* FIFO must be freed explicitly; wrap cleanup to capture it too. */
    struct FifoGuard {
        AVAudioFifo* f;
        ~FifoGuard() { if (f) av_audio_fifo_free(f); }
    } fifo_guard{afifo};

    /* --- Main read-decode-encode loop ------------------------------------ */
    PacketPtr pkt(av_packet_alloc());
    FramePtr  dec_frame(av_frame_alloc());
    FramePtr  aout_frame;  /* allocated lazily for each encoder-sized chunk */

    const int64_t total_duration = (ifmt->duration > 0) ? ifmt->duration : 0;
    int64_t next_audio_pts = 0;       /* cumulative output-tb PTS for audio */
    me_status_t terminal = ME_OK;
    std::string stage_err;

    auto drain_audio_fifo = [&](bool flush) -> me_status_t {
        while (afifo && aenc) {
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
            if (r < 0) return fail(ME_E_OUT_OF_MEMORY, "frame_get_buffer(audio): " + av_err_str(r));
            r = av_audio_fifo_read(afifo, (void**)out_af->data, this_frame);
            if (r < 0) return fail(ME_E_INTERNAL, "audio_fifo_read: " + av_err_str(r));
            out_af->pts = next_audio_pts;
            next_audio_pts += this_frame;

            me_status_t s = encode_audio_frame(out_af.get(), aenc.get(), ofmt, out_aidx, err);
            if (s != ME_OK) return s;
        }
        return ME_OK;
    };

    while (true) {
        if (opts.cancel && opts.cancel->load(std::memory_order_acquire)) {
            terminal = ME_E_CANCELLED;
            break;
        }
        rc = av_read_frame(ifmt, pkt.get());
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            terminal = ME_E_DECODE;
            if (err) *err = "read_frame: " + av_err_str(rc);
            break;
        }

        const int si = pkt->stream_index;
        AVCodecContext* dec_ctx = nullptr;
        AVRational in_tb{0, 1};
        bool is_video = false;

        if (si == vsi && vdec) { dec_ctx = vdec.get(); in_tb = ifmt->streams[vsi]->time_base; is_video = true; }
        else if (si == asi && adec) { dec_ctx = adec.get(); in_tb = ifmt->streams[asi]->time_base; }
        else { av_packet_unref(pkt.get()); continue; }

        rc = avcodec_send_packet(dec_ctx, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            terminal = ME_E_DECODE;
            if (err) *err = "send_packet: " + av_err_str(rc);
            break;
        }

        while (true) {
            rc = avcodec_receive_frame(dec_ctx, dec_frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) {
                terminal = ME_E_DECODE;
                if (err) *err = "receive_frame: " + av_err_str(rc);
                break;
            }

            if (is_video) {
                me_status_t s = encode_video_frame(dec_frame.get(), venc.get(),
                                                   sws.get(), v_scratch.get(),
                                                   ofmt, out_vidx, in_tb, err);
                if (s != ME_OK) { terminal = s; break; }

                if (opts.on_ratio && total_duration > 0 && dec_frame->pts != AV_NOPTS_VALUE) {
                    const int64_t pts_us = av_rescale_q(dec_frame->pts, in_tb, AV_TIME_BASE_Q);
                    float ratio = (float)pts_us / (float)total_duration;
                    if (ratio < 0.f) ratio = 0.f;
                    if (ratio > 1.f) ratio = 1.f;
                    opts.on_ratio(ratio);
                }
            } else {
                /* Resample → FIFO → drain encoder-sized chunks. */
                const int in_samples = dec_frame->nb_samples;
                const int out_samples_est =
                    (int)av_rescale_rnd(swr_get_delay(swr.get(), adec->sample_rate) + in_samples,
                                        aenc->sample_rate, adec->sample_rate, AV_ROUND_UP);
                std::vector<uint8_t*> out_ptrs(aenc->ch_layout.nb_channels, nullptr);
                uint8_t** out_data = nullptr;
                int out_linesize = 0;
                int r = av_samples_alloc_array_and_samples(&out_data, &out_linesize,
                                                            aenc->ch_layout.nb_channels,
                                                            out_samples_est, aenc->sample_fmt, 0);
                if (r < 0) { terminal = ME_E_OUT_OF_MEMORY; if (err) *err = "samples_alloc: " + av_err_str(r); break; }
                int converted = swr_convert(swr.get(), out_data, out_samples_est,
                                            (const uint8_t**)dec_frame->data, in_samples);
                if (converted < 0) {
                    if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
                    terminal = ME_E_INTERNAL; if (err) *err = "swr_convert: " + av_err_str(converted);
                    break;
                }
                r = av_audio_fifo_realloc(afifo, av_audio_fifo_size(afifo) + converted);
                if (r < 0) {
                    if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
                    terminal = ME_E_OUT_OF_MEMORY; if (err) *err = "fifo_realloc: " + av_err_str(r);
                    break;
                }
                r = av_audio_fifo_write(afifo, (void**)out_data, converted);
                if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
                if (r < 0) { terminal = ME_E_INTERNAL; if (err) *err = "fifo_write: " + av_err_str(r); break; }

                me_status_t s = drain_audio_fifo(false);
                if (s != ME_OK) { terminal = s; break; }
            }
            av_frame_unref(dec_frame.get());
        }

        if (terminal != ME_OK) break;
    }

    /* --- Flush decoders → encoders --------------------------------------- */
    if (terminal == ME_OK && vdec) {
        rc = avcodec_send_packet(vdec.get(), nullptr);
        if (rc >= 0) {
            while (true) {
                rc = avcodec_receive_frame(vdec.get(), dec_frame.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
                if (rc < 0) { terminal = ME_E_DECODE; if (err) *err = "flush decode(video): " + av_err_str(rc); break; }
                me_status_t s = encode_video_frame(dec_frame.get(), venc.get(),
                                                   sws.get(), v_scratch.get(),
                                                   ofmt, out_vidx,
                                                   ifmt->streams[vsi]->time_base, err);
                av_frame_unref(dec_frame.get());
                if (s != ME_OK) { terminal = s; break; }
            }
        }
        if (terminal == ME_OK) {
            me_status_t s = encode_video_frame(nullptr, venc.get(), sws.get(), v_scratch.get(),
                                               ofmt, out_vidx, ifmt->streams[vsi]->time_base, err);
            if (s != ME_OK) terminal = s;
        }
    }

    if (terminal == ME_OK && adec) {
        rc = avcodec_send_packet(adec.get(), nullptr);
        if (rc >= 0) {
            while (true) {
                rc = avcodec_receive_frame(adec.get(), dec_frame.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
                if (rc < 0) { terminal = ME_E_DECODE; if (err) *err = "flush decode(audio): " + av_err_str(rc); break; }
                const int in_samples = dec_frame->nb_samples;
                const int out_samples_est =
                    (int)av_rescale_rnd(swr_get_delay(swr.get(), adec->sample_rate) + in_samples,
                                        aenc->sample_rate, adec->sample_rate, AV_ROUND_UP);
                uint8_t** out_data = nullptr;
                int out_linesize = 0;
                int r = av_samples_alloc_array_and_samples(&out_data, &out_linesize,
                                                            aenc->ch_layout.nb_channels,
                                                            out_samples_est, aenc->sample_fmt, 0);
                if (r < 0) { terminal = ME_E_OUT_OF_MEMORY; if (err) *err = "samples_alloc(flush): " + av_err_str(r); break; }
                int converted = swr_convert(swr.get(), out_data, out_samples_est,
                                            (const uint8_t**)dec_frame->data, in_samples);
                if (converted < 0) {
                    if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
                    terminal = ME_E_INTERNAL; if (err) *err = "swr_convert(flush): " + av_err_str(converted);
                    break;
                }
                int fr = av_audio_fifo_realloc(afifo, av_audio_fifo_size(afifo) + converted);
                if (fr < 0) {
                    if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
                    terminal = ME_E_OUT_OF_MEMORY; if (err) *err = "fifo_realloc(flush): " + av_err_str(fr);
                    break;
                }
                int fw = av_audio_fifo_write(afifo, (void**)out_data, converted);
                if (fw < 0) {
                    if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
                    terminal = ME_E_INTERNAL; if (err) *err = "fifo_write(flush): " + av_err_str(fw);
                    break;
                }
                if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
                av_frame_unref(dec_frame.get());
                me_status_t s = drain_audio_fifo(false);
                if (s != ME_OK) { terminal = s; break; }
            }
        }
        /* Flush remaining FIFO samples. */
        if (terminal == ME_OK) {
            me_status_t s = drain_audio_fifo(true);
            if (s != ME_OK) terminal = s;
        }
        /* Signal end-of-stream to encoder. */
        if (terminal == ME_OK) {
            me_status_t s = encode_audio_frame(nullptr, aenc.get(), ofmt, out_aidx, err);
            if (s != ME_OK) terminal = s;
        }
    }

    /* --- Trailer + cleanup ----------------------------------------------- */
    if (terminal == ME_OK) {
        rc = av_write_trailer(ofmt);
        if (rc < 0) { terminal = ME_E_ENCODE; if (err) *err = "write_trailer: " + av_err_str(rc); }
    }

    cleanup();

    if (terminal == ME_OK && opts.on_ratio) opts.on_ratio(1.0f);
    return terminal;
}

}  // namespace me::orchestrator
