#include "orchestrator/composition_thumbnailer.hpp"

#include "core/engine_impl.hpp"
#include "core/frame_impl.hpp"
#include "orchestrator/previewer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace me::orchestrator {

namespace {

struct SwsDel   { void operator()(SwsContext* p)     const noexcept { if (p) sws_freeContext(p); } };
struct FrameDel { void operator()(AVFrame* p)        const noexcept { if (p) av_frame_free(&p); } };
struct PktDel   { void operator()(AVPacket* p)       const noexcept { if (p) av_packet_free(&p); } };
struct CtxDel   { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };

using SwsPtr   = std::unique_ptr<SwsContext,     SwsDel>;
using FramePtr = std::unique_ptr<AVFrame,        FrameDel>;
using PktPtr   = std::unique_ptr<AVPacket,       PktDel>;
using CtxPtr   = std::unique_ptr<AVCodecContext, CtxDel>;

/* Scale-to-fit while preserving aspect ratio. Zero max dims mean
 * "no cap on that axis". Returned dims are >= 1; PNG encoder
 * handles odd widths, so no even-dim rounding. Never upscales. */
void compute_out_dims(int src_w, int src_h, int max_w, int max_h,
                      int& out_w, int& out_h) {
    if (max_w <= 0 && max_h <= 0) {
        out_w = src_w;
        out_h = src_h;
        return;
    }
    double sx = (max_w > 0) ? static_cast<double>(max_w) / src_w : 1.0;
    double sy = (max_h > 0) ? static_cast<double>(max_h) / src_h : 1.0;
    if (max_w <= 0) sx = sy;
    if (max_h <= 0) sy = sx;
    const double s = std::min({sx, sy, 1.0});
    out_w = std::max(1, static_cast<int>(src_w * s));
    out_h = std::max(1, static_cast<int>(src_h * s));
}

}  // namespace

me_status_t CompositionThumbnailer::png_at(me_rational_t time,
                                            int           max_width,
                                            int           max_height,
                                            uint8_t**     out_png,
                                            size_t*       out_size) {
    if (!out_png || !out_size) return ME_E_INVALID_ARG;
    *out_png  = nullptr;
    *out_size = 0;

    if (!tl_) return ME_E_INVALID_ARG;

    /* --- 1. Pull a composed RGBA8 frame at `time` via Previewer.
     * Previewer's phase-1 single-track path returns the bottom
     * track's active clip (matches what me_render_frame produces).
     * Multi-track compose through the frame server is a follow-up
     * — see previewer.cpp's frame_at comment. */
    Previewer prev(engine_, tl_);
    me_frame* raw = nullptr;
    if (const me_status_t s = prev.frame_at(time, &raw); s != ME_OK) {
        return s;
    }
    std::unique_ptr<me_frame> mf(raw);
    const int src_w = mf->width;
    const int src_h = mf->height;
    if (src_w <= 0 || src_h <= 0) return ME_E_INTERNAL;

    /* --- 2. Scale-to-fit RGBA8 → RGB24 at requested output dims. */
    int out_w = 0, out_h = 0;
    compute_out_dims(src_w, src_h, max_width, max_height, out_w, out_h);

    SwsPtr sws(sws_getContext(src_w, src_h, AV_PIX_FMT_RGBA,
                               out_w, out_h, AV_PIX_FMT_RGB24,
                               SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) return ME_E_INTERNAL;

    FramePtr rgb(av_frame_alloc());
    if (!rgb) return ME_E_OUT_OF_MEMORY;
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = out_w;
    rgb->height = out_h;
    if (av_frame_get_buffer(rgb.get(), 32) < 0) return ME_E_OUT_OF_MEMORY;

    const uint8_t* src_slices[4]  = { mf->rgba.data(), nullptr, nullptr, nullptr };
    const int      src_strides[4] = { mf->stride,       0,       0,       0       };
    if (sws_scale(sws.get(), src_slices, src_strides, 0, src_h,
                  rgb->data, rgb->linesize) < 0) {
        return ME_E_INTERNAL;
    }

    /* --- 3. PNG encode via libavcodec. One-shot ctx; no pool (this
     * path is called at most a few times per thumbnail request). */
    const AVCodec* png_enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!png_enc) return ME_E_UNSUPPORTED;
    CtxPtr enc(avcodec_alloc_context3(png_enc));
    if (!enc) return ME_E_OUT_OF_MEMORY;
    enc->width     = out_w;
    enc->height    = out_h;
    enc->pix_fmt   = AV_PIX_FMT_RGB24;
    enc->time_base = AVRational{1, 25};
    if (avcodec_open2(enc.get(), png_enc, nullptr) < 0) return ME_E_ENCODE;

    if (avcodec_send_frame(enc.get(), rgb.get()) < 0) return ME_E_ENCODE;
    avcodec_send_frame(enc.get(), nullptr);  /* flush */

    PktPtr pkt(av_packet_alloc());
    if (!pkt) return ME_E_OUT_OF_MEMORY;
    if (avcodec_receive_packet(enc.get(), pkt.get()) < 0) return ME_E_ENCODE;

    /* --- 4. Hand ownership of the PNG bytes to the caller via
     * malloc so the caller's free() (me_buffer_free) matches. */
    auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(pkt->size)));
    if (!buf) return ME_E_OUT_OF_MEMORY;
    std::memcpy(buf, pkt->data, static_cast<size_t>(pkt->size));
    *out_png  = buf;
    *out_size = static_cast<size_t>(pkt->size);
    return ME_OK;
}

}  // namespace me::orchestrator
