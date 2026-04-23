#include "orchestrator/reencode_pipeline.hpp"

#include "io/av_err.hpp"
#include "io/demux_context.hpp"
#include "io/mux_context.hpp"
#include "orchestrator/reencode_audio.hpp"
#include "orchestrator/reencode_segment.hpp"
#include "orchestrator/reencode_video.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/mathematics.h>
}

#include <memory>
#include <string>
#include <vector>

namespace me::orchestrator {

namespace {

using me::io::av_err_str;

using CodecCtxPtr = me::resource::CodecPool::Ptr;

using detail::drain_audio_fifo;
using detail::encode_audio_frame;
using detail::encode_video_frame;
using detail::open_audio_encoder;
using detail::open_decoder;
using detail::open_video_encoder;
using detail::process_segment;
using detail::SharedEncState;
using detail::total_output_us;

int best_stream(AVFormatContext* fmt, AVMediaType type) {
    return av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
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

    /* --- Open segment[0] decoders just for encoder parameter init;
     *     they get closed at the end of this scope and reopened inside
     *     process_segment(0). Decoder open is O(ms) and happens once
     *     per segment anyway, so the double-open cost is negligible
     *     compared to threading the already-opened decoders through. --- */
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

    /* FIFO must be freed explicitly; wrap cleanup so any early-return
     * path still releases it. */
    struct FifoGuard {
        AVAudioFifo* f;
        ~FifoGuard() { if (f) av_audio_fifo_free(f); }
    } fifo_guard{shared.afifo};

    if (auto s = mux->open_avio(err);    s != ME_OK) return s;
    if (auto s = mux->write_header(err); s != ME_OK) return s;

    if (opts.on_ratio) opts.on_ratio(0.0f);
    me_status_t terminal = ME_OK;

    for (std::size_t i = 0; i < opts.segments.size() && terminal == ME_OK; ++i) {
        terminal = process_segment(opts.segments[i], *opts.pool, shared, i, err);
    }

    /* --- Flush the shared encoders once, after all segments. --- */
    if (terminal == ME_OK && shared.venc) {
        me_status_t s = encode_video_frame(nullptr, shared.venc, nullptr, nullptr,
                                           ofmt, shared.out_vidx, shared.venc->time_base, err);
        if (s != ME_OK) terminal = s;
    }
    if (terminal == ME_OK && shared.aenc) {
        me_status_t s = drain_audio_fifo(shared.afifo, shared.aenc, ofmt,
                                          shared.out_aidx, &shared.next_audio_pts,
                                          /*flush=*/true, err);
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
