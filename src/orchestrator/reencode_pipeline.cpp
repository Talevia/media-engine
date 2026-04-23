#include "orchestrator/reencode_pipeline.hpp"

#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_video.hpp"

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
using CodecCtxPtr = me::resource::CodecPool::Ptr;
using FramePtr    = me::io::AvFramePtr;
using PacketPtr   = me::io::AvPacketPtr;
using SwsPtr      = me::io::SwsContextPtr;
using SwrPtr      = me::io::SwrContextPtr;

/* Pick the best video/audio stream index per libavformat conventions. */
int best_stream(AVFormatContext* fmt, AVMediaType type) {
    return av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
}

/* Pulled in so the orchestration flow below can call these by their short
 * local-namespace names — definitions live in reencode_video.cpp /
 * reencode_audio.cpp. */
using detail::open_video_encoder;
using detail::encode_video_frame;
using detail::open_audio_encoder;
using detail::encode_audio_frame;

me_status_t open_decoder(me::resource::CodecPool& pool,
                          AVStream* in_stream,
                          CodecCtxPtr& out,
                          std::string* err) {
    const AVCodec* dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!dec) {
        if (err) *err = std::string("no decoder for ") + avcodec_get_name(in_stream->codecpar->codec_id);
        return ME_E_UNSUPPORTED;
    }
    auto ctx = pool.allocate(dec);
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

    if (!opts.pool) {
        return fail(ME_E_INVALID_ARG, "reencode_mux: opts.pool is required");
    }

    const int vsi = best_stream(ifmt, AVMEDIA_TYPE_VIDEO);
    const int asi = best_stream(ifmt, AVMEDIA_TYPE_AUDIO);
    if (vsi < 0 && asi < 0) {
        return fail(ME_E_INVALID_ARG, "input has neither video nor audio");
    }

    /* --- Decoders --------------------------------------------------------- */
    CodecCtxPtr vdec, adec;
    if (vsi >= 0) {
        me_status_t s = open_decoder(*opts.pool, ifmt->streams[vsi], vdec, err);
        if (s != ME_OK) return s;
    }
    if (asi >= 0) {
        me_status_t s = open_decoder(*opts.pool, ifmt->streams[asi], adec, err);
        if (s != ME_OK) return s;
    }

    /* --- Output container + streams --------------------------------------
     * MuxContext owns the AVFormatContext lifecycle; destructor handles
     * avio_closep + free_context on any error exit below. */
    std::string open_err;
    auto mux = me::io::MuxContext::open(opts.out_path, opts.container, &open_err);
    if (!mux) return fail(ME_E_INTERNAL, std::move(open_err));
    AVFormatContext* ofmt = mux->fmt();

    int out_vidx = -1, out_aidx = -1;
    CodecCtxPtr venc, aenc;
    AVPixelFormat venc_target_pix = AV_PIX_FMT_NONE;
    int rc = 0;

    if (vdec) {
        /* Output video stream: use same time_base as input stream so PTS maps 1:1. */
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) return fail(ME_E_OUT_OF_MEMORY, "new_stream(video)");
        out_s->time_base = ifmt->streams[vsi]->time_base;

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = open_video_encoder(*opts.pool, vdec.get(),
                                           ifmt->streams[vsi]->time_base,
                                           opts.video_bitrate_bps, global_header,
                                           venc, venc_target_pix, err);
        if (s != ME_OK) return s;

        rc = avcodec_parameters_from_context(out_s->codecpar, venc.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "params_from_context(video): " + av_err_str(rc));
        out_s->avg_frame_rate = vdec->framerate;
        out_s->r_frame_rate   = vdec->framerate;
        out_vidx = out_s->index;
    }
    if (adec) {
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) return fail(ME_E_OUT_OF_MEMORY, "new_stream(audio)");

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = open_audio_encoder(*opts.pool, adec.get(),
                                           opts.audio_bitrate_bps,
                                           global_header, aenc, err);
        if (s != ME_OK) return s;
        out_s->time_base = aenc->time_base;

        rc = avcodec_parameters_from_context(out_s->codecpar, aenc.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "params_from_context(audio): " + av_err_str(rc));
        out_aidx = out_s->index;
    }

    /* --- Open output file, write header ---------------------------------- */
    if (auto s = mux->open_avio(err);    s != ME_OK) return s;
    if (auto s = mux->write_header(err); s != ME_OK) return s;

    /* --- Scratch: sws / swr / frames -------------------------------------- */
    SwsPtr sws;
    FramePtr v_scratch;      /* NV12 staging frame */
    if (vdec) {
        const AVPixelFormat src_pix = (AVPixelFormat)vdec->pix_fmt;
        if (src_pix != venc_target_pix) {
            sws.reset(sws_getContext(vdec->width, vdec->height, src_pix,
                                     venc->width, venc->height, venc_target_pix,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!sws) return fail(ME_E_INTERNAL, "sws_getContext");
            v_scratch.reset(av_frame_alloc());
            v_scratch->format = venc_target_pix;
            v_scratch->width  = venc->width;
            v_scratch->height = venc->height;
            rc = av_frame_get_buffer(v_scratch.get(), 32);
            if (rc < 0) return fail(ME_E_OUT_OF_MEMORY, "frame_get_buffer(video): " + av_err_str(rc));
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
        if (rc < 0 || !raw_swr) return fail(ME_E_INTERNAL, "swr_alloc: " + av_err_str(rc));
        swr.reset(raw_swr);
        rc = swr_init(swr.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "swr_init: " + av_err_str(rc));

        /* AAC encoder wants a fixed frame_size per call. We buffer resampled
         * samples in an FIFO and flush frame_size chunks to the encoder. */
        afifo = av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, 1);
        if (!afifo) return fail(ME_E_OUT_OF_MEMORY, "audio_fifo_alloc");
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
        if (auto s = mux->write_trailer(err); s != ME_OK) terminal = s;
    }
    /* MuxContext destructor handles avio_closep + free_context whether
     * or not write_trailer ran. */

    if (terminal == ME_OK && opts.on_ratio) opts.on_ratio(1.0f);
    return terminal;
}

}  // namespace me::orchestrator
