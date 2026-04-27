#include "compose/encode_png_kernel.hpp"

#include "graph/types.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace me::compose {

namespace {

struct SwsDel  { void operator()(SwsContext* p)     const noexcept { if (p) sws_freeContext(p); } };
struct FrDel   { void operator()(AVFrame* p)        const noexcept { if (p) av_frame_free(&p); } };
struct PktDel  { void operator()(AVPacket* p)       const noexcept { if (p) av_packet_free(&p); } };
struct EncDel  { void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); } };

using SwsPtr = std::unique_ptr<SwsContext,     SwsDel>;
using FrPtr  = std::unique_ptr<AVFrame,        FrDel>;
using PktPtr = std::unique_ptr<AVPacket,       PktDel>;
using EncPtr = std::unique_ptr<AVCodecContext, EncDel>;

me_status_t encode_png_kernel(task::TaskContext&,
                               const graph::Properties&,
                               std::span<const graph::InputValue> inputs,
                               std::span<graph::OutputSlot>       outs) {
    if (inputs.empty()) return ME_E_INVALID_ARG;
    auto* rgba_pp = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&inputs[0].v);
    if (!rgba_pp || !*rgba_pp) return ME_E_INVALID_ARG;
    const auto& rgba = **rgba_pp;
    if (rgba.width <= 0 || rgba.height <= 0 || rgba.stride == 0) return ME_E_INVALID_ARG;

    /* Step 1: sws_scale RGBA8 → RGB24 into a freshly allocated AVFrame.
     * libavcodec PNG encoder accepts RGB24 / RGBA / GRAY8 etc; RGB24 is
     * the format compose_png_at + me_thumbnail_png both used so the
     * output bytes line up bit-for-bit with pre-kernel callers. */
    SwsPtr sws(sws_getContext(rgba.width, rgba.height, AV_PIX_FMT_RGBA,
                               rgba.width, rgba.height, AV_PIX_FMT_RGB24,
                               SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) return ME_E_INTERNAL;

    FrPtr rgb(av_frame_alloc());
    if (!rgb) return ME_E_OUT_OF_MEMORY;
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = rgba.width;
    rgb->height = rgba.height;
    if (av_frame_get_buffer(rgb.get(), 32) < 0) return ME_E_OUT_OF_MEMORY;

    const uint8_t* src_slices[4]  = { rgba.rgba.data(), nullptr, nullptr, nullptr };
    const int      src_strides[4] = { static_cast<int>(rgba.stride), 0, 0, 0 };
    if (sws_scale(sws.get(), src_slices, src_strides, 0, rgba.height,
                  rgb->data, rgb->linesize) < 0) {
        return ME_E_INTERNAL;
    }

    /* Step 2: libavcodec PNG encode. One-shot context; PNG encoder
     * emits a single packet per frame. */
    const AVCodec* png_enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!png_enc) return ME_E_UNSUPPORTED;
    EncPtr enc(avcodec_alloc_context3(png_enc));
    if (!enc) return ME_E_OUT_OF_MEMORY;
    enc->width     = rgba.width;
    enc->height    = rgba.height;
    enc->pix_fmt   = AV_PIX_FMT_RGB24;
    enc->time_base = AVRational{1, 25};
    if (avcodec_open2(enc.get(), png_enc, nullptr) < 0) return ME_E_ENCODE;

    if (avcodec_send_frame(enc.get(), rgb.get()) < 0) return ME_E_ENCODE;
    avcodec_send_frame(enc.get(), nullptr);  /* flush */

    PktPtr pkt(av_packet_alloc());
    if (!pkt) return ME_E_OUT_OF_MEMORY;
    if (avcodec_receive_packet(enc.get(), pkt.get()) < 0) return ME_E_ENCODE;

    auto buf = std::make_shared<std::vector<uint8_t>>(
        static_cast<std::size_t>(pkt->size));
    std::memcpy(buf->data(), pkt->data, static_cast<std::size_t>(pkt->size));

    outs[0].v = std::move(buf);
    return ME_OK;
}

}  // namespace

void register_encode_png_kind() {
    task::KindInfo info{
        .kind           = task::TaskKindId::RenderEncodePng,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Medium,
        .time_invariant = true,
        .kernel         = encode_png_kernel,
        .input_schema   = { {"rgba", graph::TypeId::RgbaFrame} },
        .output_schema  = { {"png",  graph::TypeId::ByteBuffer} },
        .param_schema   = {},
    };
    task::register_kind(info);
}

}  // namespace me::compose
