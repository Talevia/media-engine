#include "orchestrator/compose_frame.hpp"

#include "core/engine_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "scheduler/scheduler.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace me::orchestrator {

/* ----------------------------------------------------- resolve */

me_status_t resolve_active_clip_at(const me::Timeline& tl,
                                    me_rational_t       time,
                                    ResolvedClip*       out) {
    if (!out) return ME_E_INVALID_ARG;
    if (tl.tracks.empty() || tl.clips.empty()) return ME_E_INVALID_ARG;

    /* Normalize input time. */
    if (time.den <= 0) time.den = 1;
    if (time.num <  0) time.num = 0;

    /* Phase-1 single-bottom-track lookup — multi-track compose is
     * the previewer-multi-track-compose-graph backlog item. */
    const std::string& bottom_id = tl.tracks[0].id;
    const me::Clip*    active    = nullptr;
    me_rational_t      clip_local{0, 1};

    for (const auto& c : tl.clips) {
        if (c.track_id != bottom_id) continue;

        const int64_t e_num = c.time_start.num * c.time_duration.den +
                              c.time_duration.num * c.time_start.den;
        const int64_t e_den = c.time_start.den * c.time_duration.den;

        const bool ge_start =
            time.num * c.time_start.den >= c.time_start.num * time.den;
        const bool lt_end =
            time.num * e_den < e_num * time.den;
        if (!ge_start || !lt_end) continue;

        active = &c;
        clip_local = me_rational_t{
            time.num * c.time_start.den - c.time_start.num * time.den,
            time.den * c.time_start.den,
        };
        break;
    }
    if (!active) return ME_E_NOT_FOUND;

    auto a_it = tl.assets.find(active->asset_id);
    if (a_it == tl.assets.end()) return ME_E_NOT_FOUND;

    out->uri = a_it->second.uri;
    out->source_t = me_rational_t{
        active->source_start.num * clip_local.den +
            clip_local.num * active->source_start.den,
        active->source_start.den * clip_local.den,
    };
    return ME_OK;
}

/* -------------------------------------------------- compile graph */

std::pair<graph::Graph, graph::PortRef>
compile_frame_graph(const std::string& uri,
                     me_rational_t      source_t) {
    graph::Graph::Builder b;

    /* Node 0: IoDemux(uri) → DemuxContext */
    graph::Properties demux_props;
    demux_props["uri"].v = uri;
    auto n_demux = b.add(task::TaskKindId::IoDemux,
                          std::move(demux_props), {});

    /* Node 1: IoDecodeVideo(source_t) → AVFrame.
     * source_t lives in props (not ctx.time) so the resulting graph's
     * content_hash distinguishes evaluations at different asset-local
     * moments — this is what lets the OutputCache key disambiguate
     * frames decoded from the same asset at different timestamps. */
    graph::Properties dec_props;
    dec_props["source_t_num"].v = static_cast<int64_t>(source_t.num);
    dec_props["source_t_den"].v = static_cast<int64_t>(source_t.den);
    auto n_decode = b.add(task::TaskKindId::IoDecodeVideo,
                           std::move(dec_props),
                           { graph::PortRef{n_demux, 0} });

    /* Node 2: RenderConvertRgba8 → RgbaFrameData (tightly-packed RGBA8) */
    auto n_rgba = b.add(task::TaskKindId::RenderConvertRgba8,
                         {},
                         { graph::PortRef{n_decode, 0} });

    graph::PortRef terminal{n_rgba, 0};
    b.name_terminal("rgba", terminal);
    return {std::move(b).build(), terminal};
}

/* ------------------------------------------------- compose_frame_at */

me_status_t compose_frame_at(me_engine*                                  engine,
                              const me::Timeline&                         tl,
                              me_rational_t                               time,
                              std::shared_ptr<me::graph::RgbaFrameData>*  out_rgba,
                              std::string*                                err) {
    if (!engine || !engine->scheduler || !out_rgba) return ME_E_INVALID_ARG;
    *out_rgba = nullptr;

    ResolvedClip clip;
    if (const auto s = resolve_active_clip_at(tl, time, &clip); s != ME_OK) {
        return s;
    }

    /* Graph must outlive the await — sched::EvalInstance stores it by
     * const reference. Keep `g` in this scope. */
    auto [g, term] = compile_frame_graph(clip.uri, clip.source_t);

    me::graph::EvalContext ctx;
    ctx.frames = engine->frames.get();
    ctx.codecs = engine->codecs.get();
    ctx.time   = clip.source_t;

    try {
        auto fut = engine->scheduler->evaluate_port<
                       std::shared_ptr<me::graph::RgbaFrameData>>(g, term, ctx);
        *out_rgba = fut.await();
    } catch (const std::exception& ex) {
        if (err) *err = ex.what();
        return ME_E_DECODE;
    }
    return *out_rgba ? ME_OK : ME_E_DECODE;
}

/* ---------------------------------------------------- compose_png_at */

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

me_status_t compose_png_at(me_engine*           engine,
                            const me::Timeline&  tl,
                            me_rational_t        time,
                            int                  max_width,
                            int                  max_height,
                            uint8_t**            out_png,
                            size_t*              out_size,
                            std::string*         err) {
    if (!out_png || !out_size) return ME_E_INVALID_ARG;
    *out_png  = nullptr;
    *out_size = 0;

    /* --- 1. Pull a composed RGBA8 frame at `time`. */
    std::shared_ptr<me::graph::RgbaFrameData> rgba;
    if (const auto s = compose_frame_at(engine, tl, time, &rgba, err); s != ME_OK) {
        return s;
    }
    if (!rgba) return ME_E_INTERNAL;
    const int src_w = rgba->width;
    const int src_h = rgba->height;
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

    const uint8_t* src_slices[4]  = { rgba->rgba.data(), nullptr, nullptr, nullptr };
    const int      src_strides[4] = { static_cast<int>(rgba->stride), 0, 0, 0 };
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
