/*
 * compose_frame — per-frame compose implementations: `compose_frame_at`
 * (compile + evaluate + return RGBA) and `compose_png_at` (composition-
 * level PNG).
 *
 * `compile_compose_graph` (the graph-build half of the compose-frame
 * surface) lives in `compose_compile.cpp` since the
 * `debt-split-compose-frame-cpp` cycle. Both TUs include the same
 * header (`compose_frame.hpp`), so the consumer surface is unchanged.
 */
#include "orchestrator/compose_frame.hpp"

#include "core/engine_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "scheduler/scheduler.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace me::orchestrator {

me_status_t compose_frame_at(me_engine*                                  engine,
                              const me::Timeline&                         tl,
                              me_rational_t                               time,
                              std::shared_ptr<me::graph::RgbaFrameData>*  out_rgba,
                              std::string*                                err) {
    if (!engine || !engine->scheduler || !out_rgba) return ME_E_INVALID_ARG;
    *out_rgba = nullptr;

    graph::Graph   g;
    graph::PortRef term{};
    if (const auto s = compile_compose_graph(tl, time, &g, &term); s != ME_OK) {
        return s;
    }

    me::graph::EvalContext ctx;
    ctx.frames = engine->frames.get();
    ctx.codecs = engine->codecs.get();
    ctx.time   = time;
    ctx.engine = engine;

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

namespace {

/* Aspect-preserving fit-into-bounds. Either max axis at <= 0 means
 * "no cap on that axis"; the result respects whichever cap binds.
 * Never upscales — if both caps are larger than the source, output
 * dims match source. */
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

    /* Stage 1: pull RGBA via the multi-track compose graph. */
    std::shared_ptr<me::graph::RgbaFrameData> rgba;
    if (const auto s = compose_frame_at(engine, tl, time, &rgba, err); s != ME_OK) {
        return s;
    }
    if (!rgba) return ME_E_INTERNAL;
    const int src_w = rgba->width;
    const int src_h = rgba->height;
    if (src_w <= 0 || src_h <= 0) return ME_E_INTERNAL;

    /* Stage 2: scale-to-fit via RenderAffineBlit kernel (when needed).
     * compose_png_at can't bake the scale into compile_compose_graph
     * because the source dimensions only become known after the
     * upstream Convert kernel runs — so this stage runs the kernel
     * directly, without going through the scheduler. The kernel body
     * is the same one a graph node would invoke. */
    int out_w = 0, out_h = 0;
    compute_out_dims(src_w, src_h, max_width, max_height, out_w, out_h);

    std::shared_ptr<me::graph::RgbaFrameData> resized = rgba;
    if (out_w != src_w || out_h != src_h) {
        auto blit_fn = task::best_kernel_for(
            task::TaskKindId::RenderAffineBlit, task::Affinity::Cpu);
        /* LEGIT: kernel registry has no CPU AffineBlit binding.
         * Defensive — same shape as src/api/thumbnail.cpp:131's
         * lookup. Engines built without the standard compose kind
         * registration would hit this path. */
        if (!blit_fn) return ME_E_UNSUPPORTED;

        graph::Properties bp;
        bp["dst_w"].v   = static_cast<int64_t>(out_w);
        bp["dst_h"].v   = static_cast<int64_t>(out_h);
        bp["scale_x"].v = static_cast<double>(out_w) / static_cast<double>(src_w);
        bp["scale_y"].v = static_cast<double>(out_h) / static_cast<double>(src_h);

        graph::InputValue in;
        in.v = rgba;
        graph::OutputSlot out;
        task::TaskContext ctx{};
        if (blit_fn(ctx, bp,
                    std::span<const graph::InputValue>{&in, 1},
                    std::span<graph::OutputSlot>      {&out, 1}) != ME_OK) {
            return ME_E_INTERNAL;
        }
        auto* p = std::get_if<std::shared_ptr<graph::RgbaFrameData>>(&out.v);
        if (!p || !*p) return ME_E_INTERNAL;
        resized = *p;
    }

    /* Stage 3: PNG encode via RenderEncodePng kernel. */
    auto enc_fn = task::best_kernel_for(
        task::TaskKindId::RenderEncodePng, task::Affinity::Cpu);
    /* LEGIT: kernel registry has no CPU EncodePng binding — same
     * defensive guard as src/api/thumbnail.cpp:156's lookup. */
    if (!enc_fn) return ME_E_UNSUPPORTED;

    graph::InputValue enc_in;
    enc_in.v = resized;
    graph::OutputSlot enc_out;
    task::TaskContext ctx{};
    if (enc_fn(ctx, {},
               std::span<const graph::InputValue>{&enc_in, 1},
               std::span<graph::OutputSlot>      {&enc_out, 1}) != ME_OK) {
        return ME_E_ENCODE;
    }

    auto* png_pp = std::get_if<std::shared_ptr<std::vector<uint8_t>>>(&enc_out.v);
    if (!png_pp || !*png_pp) return ME_E_ENCODE;
    const auto& png = **png_pp;

    /* Hand ownership to the caller via malloc so me_buffer_free
     * (= std::free) can release it symmetrically. */
    auto* buf = static_cast<uint8_t*>(std::malloc(png.size()));
    if (!buf) return ME_E_OUT_OF_MEMORY;
    std::memcpy(buf, png.data(), png.size());
    *out_png  = buf;
    *out_size = png.size();
    return ME_OK;
}

}  // namespace me::orchestrator
