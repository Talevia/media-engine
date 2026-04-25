/*
 * PNG encoder branch extracted from src/api/thumbnail.cpp.
 * See thumbnail_encode_png.hpp for the contract + split rationale.
 */
#include "api/thumbnail_encode_png.hpp"

#include "io/av_err.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdlib>
#include <cstring>
#include <utility>

namespace me::detail {

namespace {

using CodecCtxPtr = me::resource::CodecPool::Ptr;
using PacketPtr   = me::io::AvPacketPtr;

}  // namespace

me_status_t encode_rgb_to_png(const me::io::AvFramePtr& rgb,
                               int                       out_w,
                               int                       out_h,
                               me::resource::CodecPool*  pool,
                               std::uint8_t**            out_png,
                               std::size_t*              out_size,
                               std::string*              err) {

    const AVCodec* png_enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!png_enc) {
        if (err) *err = "PNG encoder not available";
        /* LEGIT: PNG encoder is always shipped with stock FFmpeg, but
         * custom builds may strip it; this path surfaces that cleanly
         * instead of crashing in the encoder open call. */
        return ME_E_UNSUPPORTED;
    }
    CodecCtxPtr enc = pool
        ? pool->allocate(png_enc)
        : CodecCtxPtr{nullptr, me::resource::CodecPool::Deleter{nullptr}};
    if (!enc) return ME_E_OUT_OF_MEMORY;
    enc->width      = out_w;
    enc->height     = out_h;
    enc->pix_fmt    = AV_PIX_FMT_RGB24;
    enc->time_base  = AVRational{1, 25};         /* PNG is single image; any tb works */
    int rc = avcodec_open2(enc.get(), png_enc, nullptr);
    if (rc < 0) {
        if (err) *err = "open PNG encoder: " + me::io::av_err_str(rc);
        return ME_E_ENCODE;
    }

    rc = avcodec_send_frame(enc.get(), rgb.get());
    if (rc < 0) {
        if (err) *err = "send_frame(png): " + me::io::av_err_str(rc);
        return ME_E_ENCODE;
    }
    /* Flush so PNG encoder emits its single packet. */
    avcodec_send_frame(enc.get(), nullptr);

    PacketPtr pkt(av_packet_alloc());
    rc = avcodec_receive_packet(enc.get(), pkt.get());
    if (rc < 0) {
        if (err) *err = "receive_packet(png): " + me::io::av_err_str(rc);
        return ME_E_ENCODE;
    }

    /* Copy PNG bytes into a malloc'd buffer so me_buffer_free
     * (→ std::free) can release it symmetrically. */
    auto* buf = static_cast<std::uint8_t*>(
        std::malloc(static_cast<std::size_t>(pkt->size)));
    if (!buf) return ME_E_OUT_OF_MEMORY;
    std::memcpy(buf, pkt->data, static_cast<std::size_t>(pkt->size));
    *out_png  = buf;
    *out_size = static_cast<std::size_t>(pkt->size);
    return ME_OK;
}

}  // namespace me::detail
