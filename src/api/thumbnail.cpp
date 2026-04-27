#include "media_engine/thumbnail.h"

#include "core/engine_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/eval_error.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "scheduler/scheduler.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

/* Given native W×H and user's max bounding box, derive output dims that
 * preserve aspect ratio. 0 in a bound means "unconstrained on this axis".
 * Both 0 → native passthrough. */
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

/* Build the 3-node decode graph for an asset URI:
 *   IoDemux(uri) → IoDecodeVideo(source_t) → RenderConvertRgba8
 * Same shape as compose_frame's M1 path. The IoDecodeVideo kernel's
 * "hit-or-keep" decode policy already matches the legacy
 * decode_first_frame_at_or_after semantics — seek backward, decode
 * forward, return the first frame whose pts >= target_pts_stb (or
 * the last frame seen if EOF). */
std::pair<me::graph::Graph, me::graph::PortRef>
build_thumbnail_graph(const std::string& uri, me_rational_t source_t) {
    me::graph::Graph::Builder b;

    me::graph::Properties demux_props;
    demux_props["uri"].v = uri;
    auto n_demux = b.add(me::task::TaskKindId::IoDemux,
                          std::move(demux_props), {});

    me::graph::Properties dec_props;
    dec_props["source_t_num"].v = static_cast<int64_t>(source_t.num);
    dec_props["source_t_den"].v = static_cast<int64_t>(source_t.den);
    auto n_decode = b.add(me::task::TaskKindId::IoDecodeVideo,
                           std::move(dec_props),
                           { me::graph::PortRef{n_demux, 0} });

    auto n_rgba = b.add(me::task::TaskKindId::RenderConvertRgba8,
                         {},
                         { me::graph::PortRef{n_decode, 0} });

    me::graph::PortRef terminal{n_rgba, 0};
    b.name_terminal("rgba", terminal);
    return {std::move(b).build(), terminal};
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
    if (!engine->scheduler) return ME_E_INVALID_ARG;

    me::detail::clear_error(engine);

    me_rational_t t = time;
    if (t.den <= 0) t.den = 1;
    if (t.num <  0) t.num = 0;

    try {
        /* Stage 1: decode + convert to RGBA8 via graph. The IoDemux +
         * IoDecodeVideo + RenderConvertRgba8 chain replaces the
         * inline avformat_open_input + decode_first_frame_at_or_after
         * + sws_scale that lived here pre-graph migration. */
        auto [graph, terminal] = build_thumbnail_graph(std::string{uri}, t);

        me::graph::EvalContext ctx;
        ctx.frames = engine->frames.get();
        ctx.codecs = engine->codecs.get();
        ctx.time   = t;

        std::shared_ptr<me::graph::RgbaFrameData> rgba;
        try {
            auto fut = engine->scheduler->evaluate_port<
                           std::shared_ptr<me::graph::RgbaFrameData>>(graph, terminal, ctx);
            rgba = fut.await();
        } catch (const me::graph::EvalError& ex) {
            /* Preserve the kernel's status code (ME_E_IO from
             * avformat_open_input failure, ME_E_DECODE from
             * find_stream_info, etc.) and the kernel's message —
             * which includes "avformat_open_input(\"<path>\")"
             * for the IO case so test_thumbnail's last_error check
             * matches. */
            me::detail::set_error(engine, std::string{ex.what()});
            return ex.status();
        } catch (const std::exception& ex) {
            me::detail::set_error(engine, std::string{ex.what()});
            return ME_E_DECODE;
        }
        if (!rgba) return ME_E_DECODE;

        const int native_w = rgba->width;
        const int native_h = rgba->height;
        if (native_w <= 0 || native_h <= 0) return ME_E_DECODE;

        int out_w = 0, out_h = 0;
        fit_bounds(native_w, native_h, max_width, max_height, out_w, out_h);

        /* Stage 2: scale-to-fit via RenderAffineBlit kernel, when needed.
         * Source dims are only known after the graph runs, so the
         * AffineBlit can't be baked into compile time. Direct kernel
         * call (no scheduler) is equivalent semantics. */
        std::shared_ptr<me::graph::RgbaFrameData> resized = rgba;
        if (out_w != native_w || out_h != native_h) {
            auto blit_fn = me::task::best_kernel_for(
                me::task::TaskKindId::RenderAffineBlit, me::task::Affinity::Cpu);
            /* LEGIT: defensive — engines without the standard CPU
             * AffineBlit registration (custom embed) hit this
             * path. UNSUPPORTED is the right shape. */
            if (!blit_fn) return ME_E_UNSUPPORTED;

            me::graph::Properties bp;
            bp["dst_w"].v   = static_cast<int64_t>(out_w);
            bp["dst_h"].v   = static_cast<int64_t>(out_h);
            bp["scale_x"].v = static_cast<double>(out_w) / static_cast<double>(native_w);
            bp["scale_y"].v = static_cast<double>(out_h) / static_cast<double>(native_h);

            me::graph::InputValue in;
            in.v = rgba;
            me::graph::OutputSlot out;
            me::task::TaskContext kctx{};
            if (blit_fn(kctx, bp,
                        std::span<const me::graph::InputValue>{&in, 1},
                        std::span<me::graph::OutputSlot>      {&out, 1}) != ME_OK) {
                return ME_E_INTERNAL;
            }
            auto* p = std::get_if<std::shared_ptr<me::graph::RgbaFrameData>>(&out.v);
            if (!p || !*p) return ME_E_INTERNAL;
            resized = *p;
        }

        /* Stage 3: PNG encode via RenderEncodePng kernel. */
        auto enc_fn = me::task::best_kernel_for(
            me::task::TaskKindId::RenderEncodePng, me::task::Affinity::Cpu);
        /* LEGIT: kernel registry has no CPU EncodePng binding —
         * same defensive guard as the AffineBlit lookup above. */
        if (!enc_fn) return ME_E_UNSUPPORTED;

        me::graph::InputValue enc_in;
        enc_in.v = resized;
        me::graph::OutputSlot enc_out;
        me::task::TaskContext kctx{};
        if (enc_fn(kctx, {},
                   std::span<const me::graph::InputValue>{&enc_in, 1},
                   std::span<me::graph::OutputSlot>      {&enc_out, 1}) != ME_OK) {
            return ME_E_ENCODE;
        }

        auto* png_pp = std::get_if<std::shared_ptr<std::vector<uint8_t>>>(&enc_out.v);
        if (!png_pp || !*png_pp) return ME_E_ENCODE;
        const auto& png = **png_pp;

        auto* buf = static_cast<uint8_t*>(std::malloc(png.size()));
        if (!buf) return ME_E_OUT_OF_MEMORY;
        std::memcpy(buf, png.data(), png.size());
        *out_png  = buf;
        *out_size = png.size();
        return ME_OK;
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
