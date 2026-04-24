#include "media_engine/thumbnail.h"
#include "core/engine_impl.hpp"
#include "io/av_err.hpp"
#include "io/ffmpeg_raii.hpp"
#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

namespace {

using CodecCtxPtr = me::resource::CodecPool::Ptr;
using FramePtr    = me::io::AvFramePtr;
using PacketPtr   = me::io::AvPacketPtr;
using SwsPtr      = me::io::SwsContextPtr;

using me::io::av_err_str;

std::string strip_file_scheme(std::string_view uri) {
    constexpr std::string_view p{"file://"};
    if (uri.size() >= p.size() &&
        std::equal(p.begin(), p.end(), uri.begin())) {
        uri.remove_prefix(p.size());
    }
    return std::string{uri};
}

/* Given native W×H and user's max bounding box, derive output dims that
 * preserve aspect ratio. 0 in a bound means "unconstrained on this axis".
 * Both 0 → native passthrough. Dims are clamped to even numbers where
 * required by the encoder (PNG doesn't need this, but keeps behavior
 * predictable if we ever swap in a different codec). */
void fit_bounds(int native_w, int native_h, int max_w, int max_h,
                int& out_w, int& out_h) {
    if (max_w <= 0 && max_h <= 0) { out_w = native_w; out_h = native_h; return; }
    const double wr = max_w > 0 ? static_cast<double>(max_w) / native_w : 1e99;
    const double hr = max_h > 0 ? static_cast<double>(max_h) / native_h : 1e99;
    const double r  = std::min(wr, hr);
    if (r >= 1.0) { out_w = native_w; out_h = native_h; return; }
    out_w = std::max(1, static_cast<int>(native_w * r + 0.5));
    out_h = std::max(1, static_cast<int>(native_h * r + 0.5));
}

me_status_t decode_first_frame_at_or_after(AVFormatContext*   fmt,
                                           int                vs_idx,
                                           AVCodecContext*    dec,
                                           int64_t            target_pts_stb,
                                           FramePtr&          out_frame,
                                           std::string*       err) {
    PacketPtr pkt(av_packet_alloc());
    FramePtr  frame(av_frame_alloc());

    while (true) {
        int rc = av_read_frame(fmt, pkt.get());
        if (rc == AVERROR_EOF) {
            /* Hit EOF: send flush packet to decoder to drain any buffered
             * frame that still qualifies. */
            rc = avcodec_send_packet(dec, nullptr);
            if (rc < 0 && rc != AVERROR_EOF) {
                if (err) *err = "flush decode: " + av_err_str(rc);
                return ME_E_DECODE;
            }
        } else if (rc < 0) {
            if (err) *err = "read_frame: " + av_err_str(rc);
            return ME_E_IO;
        } else {
            if (pkt->stream_index != vs_idx) {
                av_packet_unref(pkt.get());
                continue;
            }
            rc = avcodec_send_packet(dec, pkt.get());
            av_packet_unref(pkt.get());
            if (rc < 0) {
                if (err) *err = "send_packet: " + av_err_str(rc);
                return ME_E_DECODE;
            }
        }

        while (true) {
            int r = avcodec_receive_frame(dec, frame.get());
            if (r == AVERROR(EAGAIN)) break;
            if (r == AVERROR_EOF) {
                if (out_frame) return ME_OK;
                if (err) *err = "decoder drained without producing a frame";
                return ME_E_DECODE;
            }
            if (r < 0) {
                if (err) *err = "receive_frame: " + av_err_str(r);
                return ME_E_DECODE;
            }
            const int64_t frame_pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts
                                                                    : frame->best_effort_timestamp;
            if (frame_pts != AV_NOPTS_VALUE && frame_pts < target_pts_stb) {
                /* keep decoding; remember latest frame in case target is past EOF */
                out_frame.reset(av_frame_clone(frame.get()));
                av_frame_unref(frame.get());
                continue;
            }
            /* Reached target or best we'll get. */
            out_frame.reset(av_frame_clone(frame.get()));
            av_frame_unref(frame.get());
            return ME_OK;
        }
    }
}

me_status_t probe_and_render(me_engine_t*  engine,
                             const char*   uri_c,
                             me_rational_t time,
                             int           max_w_arg,
                             int           max_h_arg,
                             uint8_t**     out_png,
                             size_t*       out_size) {
    *out_png  = nullptr;
    *out_size = 0;

    const std::string path = strip_file_scheme(uri_c);

    /* --- Open input + find video stream ---------------------------------- */
    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        me::detail::set_error(engine, "avformat_open_input: " + av_err_str(rc));
        return ME_E_IO;
    }
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        me::detail::set_error(engine, "avformat_find_stream_info: " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_DECODE;
    }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        me::detail::set_error(engine, "no video stream");
        avformat_close_input(&fmt);
        /* LEGIT: source file contains no video stream — thumbnail is
         * undefined; report as unsupported with a clear message. */
        return ME_E_UNSUPPORTED;
    }

    /* --- Decoder --------------------------------------------------------- */
    AVStream* vstream = fmt->streams[vs];
    const AVCodec* dec_codec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!dec_codec) {
        me::detail::set_error(engine, std::string("no decoder for ") +
                                       avcodec_get_name(vstream->codecpar->codec_id));
        avformat_close_input(&fmt);
        /* LEGIT: FFmpeg build lacks a decoder for this codec id;
         * runtime reject with codec name in last_error. */
        return ME_E_UNSUPPORTED;
    }
    CodecCtxPtr dec = engine->codecs
        ? engine->codecs->allocate(dec_codec)
        : CodecCtxPtr{nullptr, me::resource::CodecPool::Deleter{nullptr}};
    if (!dec) { avformat_close_input(&fmt); return ME_E_OUT_OF_MEMORY; }
    rc = avcodec_parameters_to_context(dec.get(), vstream->codecpar);
    if (rc < 0) {
        me::detail::set_error(engine, "params_to_ctx: " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_INTERNAL;
    }
    dec->pkt_timebase = vstream->time_base;
    rc = avcodec_open2(dec.get(), dec_codec, nullptr);
    if (rc < 0) {
        me::detail::set_error(engine, "open decoder: " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_DECODE;
    }

    /* --- Seek to target time (AV_TIME_BASE, backwards to nearest KF) ---- */
    me_rational_t t = time;
    if (t.den <= 0) t.den = 1;
    if (t.num < 0)  t.num = 0;
    const int64_t target_us = av_rescale_q(t.num, AVRational{1, static_cast<int>(t.den)},
                                            AV_TIME_BASE_Q);
    rc = avformat_seek_file(fmt, -1, INT64_MIN, target_us, target_us,
                             AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        /* Some containers (concat demuxer, raw streams) don't seek; fall
         * back to decoding from the start if target is near zero, else
         * surface the error. */
        if (target_us > AV_TIME_BASE) {   /* > 1 s in */
            me::detail::set_error(engine, "seek failed: " + av_err_str(rc));
            avformat_close_input(&fmt);
            return ME_E_IO;
        }
    }
    avcodec_flush_buffers(dec.get());

    /* --- Decode forward until we hit a frame at/after target ------------ */
    const int64_t target_stb = av_rescale_q(target_us, AV_TIME_BASE_Q, vstream->time_base);
    FramePtr decoded;
    std::string err;
    me_status_t s = decode_first_frame_at_or_after(fmt, vs, dec.get(), target_stb,
                                                    decoded, &err);
    if (s != ME_OK) {
        me::detail::set_error(engine, std::move(err));
        avformat_close_input(&fmt);
        return s;
    }
    if (!decoded) {
        me::detail::set_error(engine, "decoder produced no frame");
        avformat_close_input(&fmt);
        return ME_E_DECODE;
    }

    const int native_w = decoded->width;
    const int native_h = decoded->height;
    int out_w = 0, out_h = 0;
    fit_bounds(native_w, native_h, max_w_arg, max_h_arg, out_w, out_h);

    /* --- Convert to RGB24 at output dims --------------------------------- */
    SwsPtr sws(sws_getContext(native_w, native_h, static_cast<AVPixelFormat>(decoded->format),
                               out_w, out_h, AV_PIX_FMT_RGB24,
                               SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) {
        me::detail::set_error(engine, "sws_getContext");
        avformat_close_input(&fmt);
        return ME_E_INTERNAL;
    }
    FramePtr rgb(av_frame_alloc());
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = out_w;
    rgb->height = out_h;
    rc = av_frame_get_buffer(rgb.get(), 32);
    if (rc < 0) {
        me::detail::set_error(engine, "frame_get_buffer(rgb): " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_OUT_OF_MEMORY;
    }
    rc = sws_scale(sws.get(), decoded->data, decoded->linesize, 0, native_h,
                    rgb->data, rgb->linesize);
    if (rc < 0) {
        me::detail::set_error(engine, "sws_scale: " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_INTERNAL;
    }

    /* --- PNG encode via libavcodec AV_CODEC_ID_PNG ----------------------- */
    const AVCodec* png_enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!png_enc) {
        me::detail::set_error(engine, "PNG encoder not available");
        avformat_close_input(&fmt);
        /* LEGIT: PNG encoder is always shipped with stock FFmpeg, but
         * custom builds may strip it; this path surfaces that cleanly
         * instead of crashing in the encoder open call. */
        return ME_E_UNSUPPORTED;
    }
    CodecCtxPtr enc = engine->codecs
        ? engine->codecs->allocate(png_enc)
        : CodecCtxPtr{nullptr, me::resource::CodecPool::Deleter{nullptr}};
    if (!enc) { avformat_close_input(&fmt); return ME_E_OUT_OF_MEMORY; }
    enc->width      = out_w;
    enc->height     = out_h;
    enc->pix_fmt    = AV_PIX_FMT_RGB24;
    enc->time_base  = AVRational{1, 25};         /* PNG is single image; any tb works */
    rc = avcodec_open2(enc.get(), png_enc, nullptr);
    if (rc < 0) {
        me::detail::set_error(engine, "open PNG encoder: " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_ENCODE;
    }

    rc = avcodec_send_frame(enc.get(), rgb.get());
    if (rc < 0) {
        me::detail::set_error(engine, "send_frame(png): " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_ENCODE;
    }
    /* Flush so PNG encoder emits its single packet. */
    avcodec_send_frame(enc.get(), nullptr);

    PacketPtr pkt(av_packet_alloc());
    rc = avcodec_receive_packet(enc.get(), pkt.get());
    if (rc < 0) {
        me::detail::set_error(engine, "receive_packet(png): " + av_err_str(rc));
        avformat_close_input(&fmt);
        return ME_E_ENCODE;
    }

    /* Copy PNG bytes into a malloc'd buffer so me_buffer_free(→ std::free)
     * can release it symmetrically. */
    auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(pkt->size)));
    if (!buf) {
        avformat_close_input(&fmt);
        return ME_E_OUT_OF_MEMORY;
    }
    std::memcpy(buf, pkt->data, static_cast<size_t>(pkt->size));
    *out_png  = buf;
    *out_size = static_cast<size_t>(pkt->size);

    avformat_close_input(&fmt);
    return ME_OK;
}

}  // namespace

extern "C" me_status_t me_thumbnail_png(
    me_engine_t*  engine,
    const char*   uri,
    me_rational_t time,
    int           max_width,
    int           max_height,
    uint8_t**     out_png,
    size_t*       out_size) {

    if (out_png)  *out_png  = nullptr;
    if (out_size) *out_size = 0;
    if (!engine || !uri || !out_png || !out_size) return ME_E_INVALID_ARG;

    me::detail::clear_error(engine);

    try {
        return probe_and_render(engine, uri, time, max_width, max_height,
                                 out_png, out_size);
    } catch (const std::bad_alloc&) {
        if (*out_png) { std::free(*out_png); *out_png = nullptr; *out_size = 0; }
        return ME_E_OUT_OF_MEMORY;
    } catch (const std::exception& ex) {
        me::detail::set_error(engine, ex.what());
        if (*out_png) { std::free(*out_png); *out_png = nullptr; *out_size = 0; }
        return ME_E_INTERNAL;
    }
}

extern "C" void me_buffer_free(uint8_t* buf) {
    std::free(buf);
}
