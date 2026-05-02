/* sticker_decoder impl. See header for the contract.
 *
 * Pipeline mirrors `me_thumbnail_png` (asset URI → libavformat
 * demux → libavcodec decode → swscale → RGBA8) but for still-image
 * stickers rather than video assets. The single-frame case is
 * simpler than thumbnail's seek-to-time path: open, read the only
 * packet, decode the only frame, scale to RGBA8.
 */
#include "compose/sticker_decoder.hpp"

#include "io/av_err.hpp"
#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <string>
#include <utility>

namespace me::compose {

namespace {

using me::io::av_err_str;
using me::io::AvCodecContextPtr;
using me::io::AvFramePtr;
using me::io::AvPacketPtr;
using me::io::SwsContextPtr;

/* Minimal AVFormatContext owner — mirrors the local pattern used
 * across api/thumbnail.cpp + io/demux_context.cpp without pulling
 * those TUs into compose/ (sticker decoding is a compose concern,
 * not an asset/thumbnail one; the dep direction matters for the
 * five-module roles in CLAUDE.md). */
struct AvFmtCtxDel {
    void operator()(AVFormatContext* p) const noexcept {
        if (p) avformat_close_input(&p);
    }
};
using AvFmtCtxPtr = std::unique_ptr<AVFormatContext, AvFmtCtxDel>;

std::string strip_file_scheme(std::string_view uri) {
    constexpr std::string_view kFile = "file://";
    if (uri.starts_with(kFile)) {
        return std::string(uri.substr(kFile.size()));
    }
    return std::string(uri);
}

}  // namespace

me_status_t decode_sticker_to_rgba8(std::string_view uri,
                                     StickerImage*    out,
                                     std::string*     err) {
    auto fail = [&](me_status_t s, std::string msg) {
        if (err) *err = "sticker_decoder: " + std::move(msg);
        return s;
    };

    if (!out) return fail(ME_E_INVALID_ARG, "out is null");
    if (uri.empty()) return fail(ME_E_INVALID_ARG, "uri is empty");

    /* Reject schemes we don't yet support — http / https / asset
     * resolvers etc. Pre-cycle all callers use file:// URIs from
     * timeline JSON; this guard is the named rejection point so
     * future schemes get an explicit "not supported" rather than
     * a libavformat surprise. */
    auto is_supported_scheme = [](std::string_view u) {
        if (u.starts_with("file://")) return true;
        if (u.starts_with("/"))       return true;   /* path-as-uri */
        if (u.starts_with("./") || u.starts_with("../")) return true;
        /* Single-segment relative paths (no scheme, no leading /)
         * are accepted too — matches host conventions for
         * timeline-relative assets. */
        if (u.find("://") == std::string_view::npos) return true;
        return false;
    };
    if (!is_supported_scheme(uri)) {
        return fail(ME_E_UNSUPPORTED,
                    "uri scheme not supported (got '" + std::string(uri) +
                    "', expected file:// or path)");
    }

    const std::string path = strip_file_scheme(uri);

    /* --- Open the still-image file via libavformat ---------- */
    AVFormatContext* fmt_raw = nullptr;
    int rc = avformat_open_input(&fmt_raw, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        return fail(ME_E_IO,
                    "open '" + path + "': " + av_err_str(rc));
    }
    AvFmtCtxPtr fmt(fmt_raw);

    rc = avformat_find_stream_info(fmt.get(), nullptr);
    if (rc < 0) {
        return fail(ME_E_DECODE,
                    "find_stream_info '" + path + "': " + av_err_str(rc));
    }
    if (fmt->nb_streams == 0) {
        return fail(ME_E_DECODE, "no streams in '" + path + "'");
    }

    AVStream* in_stream = fmt->streams[0];
    if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        return fail(ME_E_DECODE,
                    "first stream is not a video/image stream in '" +
                    path + "'");
    }

    /* --- Open the matching libavcodec decoder --------------- */
    const AVCodec* dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!dec) {
        return fail(ME_E_UNSUPPORTED,
                    "no decoder for codec_id=" +
                    std::to_string(static_cast<int>(in_stream->codecpar->codec_id)) +
                    " (image '" + path + "')");
    }
    AvCodecContextPtr dec_ctx(avcodec_alloc_context3(dec));
    if (!dec_ctx) return fail(ME_E_OUT_OF_MEMORY, "alloc decoder context");
    rc = avcodec_parameters_to_context(dec_ctx.get(), in_stream->codecpar);
    if (rc < 0) {
        return fail(ME_E_INTERNAL,
                    "parameters_to_context: " + av_err_str(rc));
    }
    rc = avcodec_open2(dec_ctx.get(), dec, nullptr);
    if (rc < 0) {
        return fail(ME_E_DECODE, "open decoder: " + av_err_str(rc));
    }

    /* --- Read the single packet + decode the single frame --- */
    AvPacketPtr pkt(av_packet_alloc());
    AvFramePtr  dec_frame(av_frame_alloc());
    if (!pkt || !dec_frame) {
        return fail(ME_E_OUT_OF_MEMORY, "pkt/frame alloc");
    }
    rc = av_read_frame(fmt.get(), pkt.get());
    if (rc < 0) {
        return fail(ME_E_DECODE, "read_frame: " + av_err_str(rc));
    }
    rc = avcodec_send_packet(dec_ctx.get(), pkt.get());
    av_packet_unref(pkt.get());
    if (rc < 0 && rc != AVERROR_EOF) {
        return fail(ME_E_DECODE, "send_packet: " + av_err_str(rc));
    }
    /* Drain the decoder; one frame for a still image. Some
     * libavcodec decoders (PNG) deliver immediately; others (WebP)
     * may need a flush. We send EOF after the first packet to
     * cover both cases. */
    rc = avcodec_send_packet(dec_ctx.get(), nullptr);
    if (rc < 0 && rc != AVERROR_EOF) {
        return fail(ME_E_DECODE, "send_packet(EOF): " + av_err_str(rc));
    }
    rc = avcodec_receive_frame(dec_ctx.get(), dec_frame.get());
    if (rc < 0) {
        return fail(ME_E_DECODE, "receive_frame: " + av_err_str(rc));
    }

    const int w = dec_frame->width;
    const int h = dec_frame->height;
    if (w <= 0 || h <= 0) {
        return fail(ME_E_DECODE,
                    "decoded frame has non-positive dimensions");
    }

    /* --- swscale to RGBA8 ----------------------------------- */
    SwsContextPtr sws(sws_getContext(
        w, h, static_cast<AVPixelFormat>(dec_frame->format),
        w, h, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) return fail(ME_E_INTERNAL, "sws_getContext");

    out->width  = w;
    out->height = h;
    out->pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);

    std::uint8_t* dst_planes[4]   = { out->pixels.data(), nullptr, nullptr, nullptr };
    int           dst_strides[4]  = { w * 4,              0,       0,       0       };
    rc = sws_scale(sws.get(), dec_frame->data, dec_frame->linesize,
                    0, h, dst_planes, dst_strides);
    if (rc < 0) {
        return fail(ME_E_INTERNAL, "sws_scale: " + av_err_str(rc));
    }
    return ME_OK;
}

}  // namespace me::compose
