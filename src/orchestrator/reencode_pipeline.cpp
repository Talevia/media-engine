#include "orchestrator/reencode_pipeline.hpp"

#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "io/ffmpeg_raii.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_video.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/audio_fifo.h>
}

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace me::orchestrator {

namespace {

using me::io::av_err_str;

/* Short file-local aliases over the shared me::io deleters — keeps the
 * dense decode/encode code below readable without repeating the full
 * namespace-qualified name every few lines. */
using CodecCtxPtr = me::resource::CodecPool::Ptr;
using FramePtr    = me::io::AvFramePtr;
using PacketPtr   = me::io::AvPacketPtr;
using SwsPtr      = me::io::SwsContextPtr;
using SwrPtr      = me::io::SwrContextPtr;

using detail::open_video_encoder;
using detail::encode_video_frame;
using detail::open_audio_encoder;
using detail::encode_audio_frame;

int best_stream(AVFormatContext* fmt, AVMediaType type) {
    return av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
}

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

/* Shared encoder / muxer state threaded through per-segment processing.
 * Bundled so process_segment's signature doesn't balloon. */
struct SharedEncState {
    AVFormatContext* ofmt          = nullptr;
    AVCodecContext*  venc          = nullptr;
    AVCodecContext*  aenc          = nullptr;
    int              out_vidx      = -1;
    int              out_aidx      = -1;
    AVPixelFormat    venc_pix      = AV_PIX_FMT_NONE;
    AVAudioFifo*     afifo         = nullptr;

    /* Fixed output video frame duration in venc->time_base — derived once
     * from segment[0]'s frame rate. Restamping each decoded frame with a
     * running counter incremented by this delta produces CFR output even
     * from VFR inputs, which is standard re-encode behavior. */
    int64_t          video_pts_delta  = 0;
    int64_t          next_video_pts   = 0;
    /* Running output-tb PTS for audio. The FIFO drain reads exactly
     * aenc->frame_size samples per encoded frame and stamps them with
     * this monotonically-incrementing counter, so segment boundaries are
     * transparent to the audio encoder — they're just more samples
     * flowing through the FIFO. */
    int64_t          next_audio_pts   = 0;

    /* Expected source params for codec_compat check across segments.
     * Populated from segment[0]'s decoders. */
    int              v_width   = 0;
    int              v_height  = 0;
    AVPixelFormat    v_pix     = AV_PIX_FMT_NONE;
    int              a_sr      = 0;
    AVSampleFormat   a_fmt     = AV_SAMPLE_FMT_NONE;
    int              a_chans   = 0;

    const std::atomic<bool>*        cancel  = nullptr;
    std::function<void(float)>      on_ratio;
    int64_t                         total_us  = 0;
};

bool video_params_compatible(const SharedEncState& s, const AVCodecContext* vdec) {
    return vdec->width == s.v_width && vdec->height == s.v_height &&
           vdec->pix_fmt == s.v_pix;
}

bool audio_params_compatible(const SharedEncState& s, const AVCodecContext* adec) {
    return adec->sample_rate == s.a_sr &&
           adec->sample_fmt  == s.a_fmt &&
           adec->ch_layout.nb_channels == s.a_chans;
}

/* Estimate total output duration (microseconds) summed across segments,
 * for progress ratio reporting. source_duration == 0 → fallback to demux
 * duration minus source_start. */
int64_t total_output_us(const std::vector<ReencodeSegment>& segs) {
    int64_t total = 0;
    for (const auto& seg : segs) {
        if (seg.source_duration.den > 0 && seg.source_duration.num > 0) {
            total += av_rescale_q(seg.source_duration.num,
                                   AVRational{1, static_cast<int>(seg.source_duration.den)},
                                   AV_TIME_BASE_Q);
        } else if (seg.demux && seg.demux->fmt && seg.demux->fmt->duration > 0) {
            int64_t src_start_us = 0;
            if (seg.source_start.den > 0 && seg.source_start.num > 0) {
                src_start_us = av_rescale_q(seg.source_start.num,
                                             AVRational{1, static_cast<int>(seg.source_start.den)},
                                             AV_TIME_BASE_Q);
            }
            total += std::max<int64_t>(0, seg.demux->fmt->duration - src_start_us);
        }
    }
    return total;
}

/* Drain the shared audio FIFO in encoder-sized chunks, stamping each
 * encoded frame with the cross-segment next_audio_pts. flush=true allows
 * a final short frame. */
me_status_t drain_audio_fifo(SharedEncState& s, bool flush, std::string* err) {
    while (s.afifo && s.aenc) {
        const int have = av_audio_fifo_size(s.afifo);
        const int need = s.aenc->frame_size;
        if (!flush && have < need) return ME_OK;
        if (flush && have == 0)    return ME_OK;

        const int this_frame = flush ? std::min(have, need ? need : have) : need;
        FramePtr out_af(av_frame_alloc());
        out_af->nb_samples = this_frame;
        out_af->format     = s.aenc->sample_fmt;
        av_channel_layout_copy(&out_af->ch_layout, &s.aenc->ch_layout);
        out_af->sample_rate = s.aenc->sample_rate;
        int r = av_frame_get_buffer(out_af.get(), 0);
        if (r < 0) {
            if (err) *err = "frame_get_buffer(audio): " + av_err_str(r);
            return ME_E_OUT_OF_MEMORY;
        }
        r = av_audio_fifo_read(s.afifo, (void**)out_af->data, this_frame);
        if (r < 0) {
            if (err) *err = "audio_fifo_read: " + av_err_str(r);
            return ME_E_INTERNAL;
        }
        out_af->pts = s.next_audio_pts;
        s.next_audio_pts += this_frame;

        me_status_t st = encode_audio_frame(out_af.get(), s.aenc, s.ofmt, s.out_aidx, err);
        if (st != ME_OK) return st;
    }
    return ME_OK;
}

/* Process one segment: open its decoders, seek, loop read→decode→encode
 * (feeding the SHARED encoder), flush the decoders. The encoder is NOT
 * flushed here — that happens once at the end of the full segment list. */
me_status_t process_segment(const ReencodeSegment&   seg,
                             me::resource::CodecPool& pool,
                             SharedEncState&          shared,
                             size_t                   seg_idx,
                             std::string*             err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = "segment[" + std::to_string(seg_idx) + "] " + std::move(msg);
        return s;
    };

    AVFormatContext* ifmt = seg.demux ? seg.demux->fmt : nullptr;
    if (!ifmt) return fail(ME_E_INVALID_ARG, "no demux context");

    const int vsi = best_stream(ifmt, AVMEDIA_TYPE_VIDEO);
    const int asi = best_stream(ifmt, AVMEDIA_TYPE_AUDIO);

    /* --- Per-segment decoders --- */
    CodecCtxPtr vdec, adec;
    if (vsi >= 0 && shared.venc) {
        me_status_t s = open_decoder(pool, ifmt->streams[vsi], vdec, err);
        if (s != ME_OK) return fail(s, "decoder(video)");
        if (!video_params_compatible(shared, vdec.get())) {
            return fail(ME_E_UNSUPPORTED,
                        "video params (w×h/pix_fmt) differ from segment[0]; "
                        "phase-1: re-encode concat requires identical video params across segments");
        }
    }
    if (asi >= 0 && shared.aenc) {
        me_status_t s = open_decoder(pool, ifmt->streams[asi], adec, err);
        if (s != ME_OK) return fail(s, "decoder(audio)");
        if (!audio_params_compatible(shared, adec.get())) {
            return fail(ME_E_UNSUPPORTED,
                        "audio params (sr/sample_fmt/channels) differ from segment[0]; "
                        "phase-1: re-encode concat requires identical audio params across segments");
        }
    }

    /* --- Per-segment sws/swr (source pix_fmt is stable per enforcement
     *     above, so sws target is always shared.venc_pix; swr target is
     *     always the shared encoder's sample_fmt/sample_rate). --- */
    SwsPtr sws;
    FramePtr v_scratch;
    if (vdec && vdec->pix_fmt != shared.venc_pix) {
        sws.reset(sws_getContext(vdec->width, vdec->height, (AVPixelFormat)vdec->pix_fmt,
                                 shared.venc->width, shared.venc->height, shared.venc_pix,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) return fail(ME_E_INTERNAL, "sws_getContext");
        v_scratch.reset(av_frame_alloc());
        v_scratch->format = shared.venc_pix;
        v_scratch->width  = shared.venc->width;
        v_scratch->height = shared.venc->height;
        int rc = av_frame_get_buffer(v_scratch.get(), 32);
        if (rc < 0) return fail(ME_E_OUT_OF_MEMORY, "frame_get_buffer(video): " + av_err_str(rc));
    }

    SwrPtr swr;
    if (adec) {
        SwrContext* raw_swr = nullptr;
        int rc = swr_alloc_set_opts2(&raw_swr,
                                     &shared.aenc->ch_layout, shared.aenc->sample_fmt, shared.aenc->sample_rate,
                                     &adec->ch_layout, adec->sample_fmt, adec->sample_rate,
                                     0, nullptr);
        if (rc < 0 || !raw_swr) return fail(ME_E_INTERNAL, "swr_alloc: " + av_err_str(rc));
        swr.reset(raw_swr);
        rc = swr_init(swr.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "swr_init: " + av_err_str(rc));
    }

    /* --- Seek --- */
    int rc = 0;
    const int64_t src_start_us = (seg.source_start.den > 0 && seg.source_start.num > 0)
        ? av_rescale_q(seg.source_start.num,
                        AVRational{1, static_cast<int>(seg.source_start.den)},
                        AV_TIME_BASE_Q)
        : 0;
    if (src_start_us > 0) {
        rc = avformat_seek_file(ifmt, -1, INT64_MIN, src_start_us, src_start_us,
                                 AVSEEK_FLAG_BACKWARD);
        if (rc < 0) return fail(ME_E_IO, "seek: " + av_err_str(rc));
    }
    int64_t src_end_us = INT64_MAX;
    if (seg.source_duration.den > 0 && seg.source_duration.num > 0) {
        const int64_t dur_us = av_rescale_q(seg.source_duration.num,
                                             AVRational{1, static_cast<int>(seg.source_duration.den)},
                                             AV_TIME_BASE_Q);
        src_end_us = src_start_us + dur_us;
    }

    /* --- Read/decode/encode loop --- */
    PacketPtr pkt(av_packet_alloc());
    FramePtr  dec_frame(av_frame_alloc());
    me_status_t terminal = ME_OK;

    /* Feed decoded video frames to the encoder with shared.next_video_pts
     * stamped directly in venc->time_base units. encode_video_frame rescales
     * via its `in_stream_tb` arg; handing it venc->time_base makes that rescale
     * an identity. */
    auto push_video_frame = [&](AVFrame* f) -> me_status_t {
        if (f) {
            f->pts     = shared.next_video_pts;
            f->pkt_dts = shared.next_video_pts;
            shared.next_video_pts += shared.video_pts_delta;
        }
        return encode_video_frame(f, shared.venc, sws.get(), v_scratch.get(),
                                   shared.ofmt, shared.out_vidx,
                                   shared.venc->time_base, err);
    };

    auto push_audio_frame = [&](AVFrame* f) -> me_status_t {
        if (!f) {
            /* Upstream flush: drain the per-clip swr_convert residual then
             * push whatever's left in the FIFO. */
            std::vector<uint8_t*> out_ptrs(shared.aenc->ch_layout.nb_channels, nullptr);
            uint8_t** out_data = nullptr;
            int out_linesize = 0;
            const int out_samples_est =
                (int)av_rescale_rnd(swr_get_delay(swr.get(), adec->sample_rate),
                                    shared.aenc->sample_rate, adec->sample_rate, AV_ROUND_UP);
            if (out_samples_est > 0) {
                int r = av_samples_alloc_array_and_samples(&out_data, &out_linesize,
                                                            shared.aenc->ch_layout.nb_channels,
                                                            out_samples_est, shared.aenc->sample_fmt, 0);
                if (r < 0) return fail(ME_E_OUT_OF_MEMORY, "samples_alloc(flush): " + av_err_str(r));
                int converted = swr_convert(swr.get(), out_data, out_samples_est, nullptr, 0);
                if (converted > 0) {
                    int fr = av_audio_fifo_realloc(shared.afifo,
                                                    av_audio_fifo_size(shared.afifo) + converted);
                    if (fr >= 0) av_audio_fifo_write(shared.afifo, (void**)out_data, converted);
                }
                if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
            }
            return ME_OK;
        }

        const int in_samples = f->nb_samples;
        const int out_samples_est =
            (int)av_rescale_rnd(swr_get_delay(swr.get(), adec->sample_rate) + in_samples,
                                shared.aenc->sample_rate, adec->sample_rate, AV_ROUND_UP);
        uint8_t** out_data = nullptr;
        int out_linesize = 0;
        int r = av_samples_alloc_array_and_samples(&out_data, &out_linesize,
                                                    shared.aenc->ch_layout.nb_channels,
                                                    out_samples_est, shared.aenc->sample_fmt, 0);
        if (r < 0) return fail(ME_E_OUT_OF_MEMORY, "samples_alloc: " + av_err_str(r));
        int converted = swr_convert(swr.get(), out_data, out_samples_est,
                                    (const uint8_t**)f->data, in_samples);
        if (converted < 0) {
            if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
            return fail(ME_E_INTERNAL, "swr_convert: " + av_err_str(converted));
        }
        r = av_audio_fifo_realloc(shared.afifo, av_audio_fifo_size(shared.afifo) + converted);
        if (r < 0) {
            if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
            return fail(ME_E_OUT_OF_MEMORY, "fifo_realloc: " + av_err_str(r));
        }
        r = av_audio_fifo_write(shared.afifo, (void**)out_data, converted);
        if (out_data) { av_freep(&out_data[0]); av_freep(&out_data); }
        if (r < 0) return fail(ME_E_INTERNAL, "fifo_write: " + av_err_str(r));

        return drain_audio_fifo(shared, false, err);
    };

    while (terminal == ME_OK) {
        if (shared.cancel && shared.cancel->load(std::memory_order_acquire)) {
            terminal = ME_E_CANCELLED;
            break;
        }
        rc = av_read_frame(ifmt, pkt.get());
        if (rc == AVERROR_EOF) break;
        if (rc < 0) { terminal = fail(ME_E_DECODE, "read_frame: " + av_err_str(rc)); break; }

        const int si = pkt->stream_index;
        AVCodecContext* dec_ctx = nullptr;
        AVRational in_tb{0, 1};
        bool is_video = false;

        if (si == vsi && vdec) { dec_ctx = vdec.get(); in_tb = ifmt->streams[vsi]->time_base; is_video = true; }
        else if (si == asi && adec) { dec_ctx = adec.get(); in_tb = ifmt->streams[asi]->time_base; }
        else { av_packet_unref(pkt.get()); continue; }

        /* Stop this segment once past source_duration (compare in source tb). */
        if (pkt->pts != AV_NOPTS_VALUE) {
            const int64_t pts_us = av_rescale_q(pkt->pts, in_tb, AV_TIME_BASE_Q);
            if (pts_us >= src_end_us) {
                av_packet_unref(pkt.get());
                break;
            }
        }

        rc = avcodec_send_packet(dec_ctx, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) { terminal = fail(ME_E_DECODE, "send_packet: " + av_err_str(rc)); break; }

        while (true) {
            rc = avcodec_receive_frame(dec_ctx, dec_frame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) { terminal = fail(ME_E_DECODE, "receive_frame: " + av_err_str(rc)); break; }

            me_status_t s = is_video ? push_video_frame(dec_frame.get())
                                      : push_audio_frame(dec_frame.get());
            av_frame_unref(dec_frame.get());
            if (s != ME_OK) { terminal = s; break; }

            /* Progress — next_video_pts is monotonic across all segments
             * (single shared encoder), so rescaling it to AV_TIME_BASE_Q
             * gives the cumulative output duration directly without a
             * separate per-segment accumulator. */
            if (is_video && shared.on_ratio && shared.total_us > 0) {
                const int64_t cum_us = av_rescale_q(shared.next_video_pts,
                                                     shared.venc->time_base,
                                                     AV_TIME_BASE_Q);
                float ratio = static_cast<float>(cum_us) / static_cast<float>(shared.total_us);
                if (ratio < 0.f) ratio = 0.f;
                if (ratio > 1.f) ratio = 1.f;
                shared.on_ratio(ratio);
            }
        }
        if (terminal != ME_OK) break;
    }

    /* --- End-of-segment: flush per-segment decoders into the shared encoder.
     *     Do NOT flush the shared encoder here — encoder flush only happens
     *     once, after all segments. --- */
    if (terminal == ME_OK && vdec) {
        rc = avcodec_send_packet(vdec.get(), nullptr);
        if (rc >= 0) {
            while (terminal == ME_OK) {
                rc = avcodec_receive_frame(vdec.get(), dec_frame.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
                if (rc < 0) { terminal = fail(ME_E_DECODE, "flush decode(video): " + av_err_str(rc)); break; }
                me_status_t s = push_video_frame(dec_frame.get());
                av_frame_unref(dec_frame.get());
                if (s != ME_OK) { terminal = s; break; }
            }
        }
    }
    if (terminal == ME_OK && adec) {
        rc = avcodec_send_packet(adec.get(), nullptr);
        if (rc >= 0) {
            while (terminal == ME_OK) {
                rc = avcodec_receive_frame(adec.get(), dec_frame.get());
                if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
                if (rc < 0) { terminal = fail(ME_E_DECODE, "flush decode(audio): " + av_err_str(rc)); break; }
                me_status_t s = push_audio_frame(dec_frame.get());
                av_frame_unref(dec_frame.get());
                if (s != ME_OK) { terminal = s; break; }
            }
        }
        /* Flush residual swr state (no more input samples) and push
         * converted leftovers into the FIFO. */
        if (terminal == ME_OK) {
            me_status_t s = push_audio_frame(nullptr);
            if (s != ME_OK) terminal = s;
        }
    }

    return terminal;
}

}  // namespace

me_status_t reencode_mux(const ReencodeOptions& opts,
                         std::string*           err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = std::move(msg);
        return s;
    };

    if (opts.video_codec != "h264") {
        return fail(ME_E_UNSUPPORTED,
                    "video_codec=\"" + opts.video_codec + "\" not supported (expected \"h264\")");
    }
    if (opts.audio_codec != "aac") {
        return fail(ME_E_UNSUPPORTED,
                    "audio_codec=\"" + opts.audio_codec + "\" not supported (expected \"aac\")");
    }
    if (!opts.pool) return fail(ME_E_INVALID_ARG, "reencode_mux: opts.pool is required");
    if (opts.segments.empty()) return fail(ME_E_INVALID_ARG, "reencode_mux: segments is empty");

    AVFormatContext* ifmt0 = opts.segments.front().demux ? opts.segments.front().demux->fmt : nullptr;
    if (!ifmt0) return fail(ME_E_INVALID_ARG, "segment[0] has no demux context");

    const int vsi0 = best_stream(ifmt0, AVMEDIA_TYPE_VIDEO);
    const int asi0 = best_stream(ifmt0, AVMEDIA_TYPE_AUDIO);
    if (vsi0 < 0 && asi0 < 0) return fail(ME_E_INVALID_ARG, "segment[0] has neither video nor audio");

    /* --- Open segment[0] decoders just for encoder parameter init; they
     *     get closed at the end of this scope and reopened inside
     *     process_segment(0). This is cheaper than trying to thread the
     *     already-opened decoders into process_segment — decoder open is
     *     O(ms) and happens once per segment anyway. --- */
    CodecCtxPtr v0dec, a0dec;
    if (vsi0 >= 0) {
        me_status_t s = open_decoder(*opts.pool, ifmt0->streams[vsi0], v0dec, err);
        if (s != ME_OK) return s;
    }
    if (asi0 >= 0) {
        me_status_t s = open_decoder(*opts.pool, ifmt0->streams[asi0], a0dec, err);
        if (s != ME_OK) return s;
    }

    std::string open_err;
    auto mux = me::io::MuxContext::open(opts.out_path, opts.container, &open_err);
    if (!mux) return fail(ME_E_INTERNAL, std::move(open_err));
    AVFormatContext* ofmt = mux->fmt();

    SharedEncState shared;
    shared.ofmt       = ofmt;
    shared.cancel     = opts.cancel;
    shared.on_ratio   = opts.on_ratio;
    shared.total_us   = total_output_us(opts.segments);

    CodecCtxPtr venc, aenc;
    int rc = 0;
    if (v0dec) {
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) return fail(ME_E_OUT_OF_MEMORY, "new_stream(video)");
        out_s->time_base = ifmt0->streams[vsi0]->time_base;

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = open_video_encoder(*opts.pool, v0dec.get(),
                                           ifmt0->streams[vsi0]->time_base,
                                           opts.video_bitrate_bps, global_header,
                                           venc, shared.venc_pix, err);
        if (s != ME_OK) return s;

        rc = avcodec_parameters_from_context(out_s->codecpar, venc.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "params_from_context(video): " + av_err_str(rc));
        out_s->avg_frame_rate = v0dec->framerate;
        out_s->r_frame_rate   = v0dec->framerate;
        shared.venc      = venc.get();
        shared.out_vidx  = out_s->index;
        shared.v_width   = v0dec->width;
        shared.v_height  = v0dec->height;
        shared.v_pix     = (AVPixelFormat)v0dec->pix_fmt;

        /* Fixed CFR delta = 1 / framerate in venc->time_base.
         * av_guess_frame_rate falls back to stream avg_frame_rate when
         * r_frame_rate is unreliable; either way we get a sane delta. */
        AVRational fr = av_guess_frame_rate(ifmt0, ifmt0->streams[vsi0], nullptr);
        if (fr.num <= 0 || fr.den <= 0) fr = AVRational{25, 1};
        shared.video_pts_delta = av_rescale_q(1, av_inv_q(fr), venc->time_base);
        if (shared.video_pts_delta <= 0) shared.video_pts_delta = 1;
    }
    if (a0dec) {
        AVStream* out_s = avformat_new_stream(ofmt, nullptr);
        if (!out_s) return fail(ME_E_OUT_OF_MEMORY, "new_stream(audio)");

        const bool global_header = (ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        me_status_t s = open_audio_encoder(*opts.pool, a0dec.get(),
                                           opts.audio_bitrate_bps,
                                           global_header, aenc, err);
        if (s != ME_OK) return s;
        out_s->time_base = aenc->time_base;

        rc = avcodec_parameters_from_context(out_s->codecpar, aenc.get());
        if (rc < 0) return fail(ME_E_INTERNAL, "params_from_context(audio): " + av_err_str(rc));
        shared.aenc     = aenc.get();
        shared.out_aidx = out_s->index;
        shared.a_sr     = a0dec->sample_rate;
        shared.a_fmt    = a0dec->sample_fmt;
        shared.a_chans  = a0dec->ch_layout.nb_channels;

        shared.afifo = av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, 1);
        if (!shared.afifo) return fail(ME_E_OUT_OF_MEMORY, "audio_fifo_alloc");
    }

    /* --- Release the parameter-sniffing segment[0] decoders before
     *     process_segment(0) reopens them. Decoder state doesn't carry
     *     across this close/open; the encoder state is what matters. --- */
    v0dec.reset();
    a0dec.reset();

    /* FIFO must be freed explicitly; wrap cleanup so any early-return path
     * still releases it. */
    struct FifoGuard {
        AVAudioFifo* f;
        ~FifoGuard() { if (f) av_audio_fifo_free(f); }
    } fifo_guard{shared.afifo};

    if (auto s = mux->open_avio(err);    s != ME_OK) return s;
    if (auto s = mux->write_header(err); s != ME_OK) return s;

    if (opts.on_ratio) opts.on_ratio(0.0f);
    me_status_t terminal = ME_OK;

    for (size_t i = 0; i < opts.segments.size() && terminal == ME_OK; ++i) {
        terminal = process_segment(opts.segments[i], *opts.pool, shared, i, err);
    }

    /* --- Flush the shared encoders once, after all segments. --- */
    if (terminal == ME_OK && shared.venc) {
        me_status_t s = encode_video_frame(nullptr, shared.venc, nullptr, nullptr,
                                           ofmt, shared.out_vidx, shared.venc->time_base, err);
        if (s != ME_OK) terminal = s;
    }
    if (terminal == ME_OK && shared.aenc) {
        me_status_t s = drain_audio_fifo(shared, true, err);
        if (s != ME_OK) terminal = s;
        if (terminal == ME_OK) {
            s = encode_audio_frame(nullptr, shared.aenc, ofmt, shared.out_aidx, err);
            if (s != ME_OK) terminal = s;
        }
    }

    if (terminal == ME_OK) {
        if (auto s = mux->write_trailer(err); s != ME_OK) terminal = s;
    }

    if (terminal == ME_OK && opts.on_ratio) opts.on_ratio(1.0f);
    return terminal;
}

}  // namespace me::orchestrator
