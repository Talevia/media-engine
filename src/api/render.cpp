#include "media_engine/render.h"
#include "core/engine_impl.hpp"
#include "core/frame_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "orchestrator/compose_frame.hpp"
#include "orchestrator/exporter.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/disk_cache.hpp"
#include "scheduler/scheduler.hpp"
#include "timeline/timeline_impl.hpp"

#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

/* me_render_job wraps the orchestrator's opaque Job — the C API shape stays
 * stable even as the orchestrator evolves. The engine pointer is stashed
 * so me_render_wait can forward worker-populated error messages into the
 * caller's thread-local last-error slot on join. */
struct me_render_job {
    me_engine_t*                                     engine = nullptr;
    std::unique_ptr<me::orchestrator::Exporter::Job> job;
};

namespace {

/* Bootstrap: Timeline ownership. me_timeline owns me::Timeline directly; the
 * orchestrator wants a shared_ptr<const Timeline>. We wrap the borrowed
 * Timeline in a shared_ptr with a no-op deleter so lifetime stays with the
 * caller's me_timeline_t handle. Real shared ownership arrives with the
 * refactor-passthrough-into-graph-exporter migration. */
std::shared_ptr<const me::Timeline> borrow_timeline(const me_timeline_t* h) {
    return std::shared_ptr<const me::Timeline>(&h->tl, [](const me::Timeline*) {});
}

/* DiskCache key derived from the compiled graph's content_hash. Same
 * (timeline-state, time) → same graph topology + props → same content
 * hash → same key. Single-track-no-transform timelines collapse to the
 * 3-node M1 graph whose hash includes the IoDemux uri prop and the
 * IoDecodeVideo source_t props, so this key is at least as specific as
 * the legacy `<asset_hash>:<source_num>:<source_den>` shape was. The
 * legacy shape supported `me_cache_invalidate_asset` prefix matching;
 * graph-hash keys lose that fast-path — me_cache_clear remains the
 * universal escape hatch. */
std::string disk_cache_key_from(const me::graph::Graph& g) {
    std::ostringstream os;
    os << "g:" << std::hex << g.content_hash();
    return os.str();
}

}  // namespace

extern "C" me_status_t me_render_start(
    me_engine_t*            engine,
    const me_timeline_t*    timeline,
    const me_output_spec_t* output,
    me_progress_cb          cb,
    void*                   user,
    me_render_job_t**       out_job) {

    if (!engine || !timeline || !output || !out_job) return ME_E_INVALID_ARG;
    *out_job = nullptr;
    me::detail::clear_error(engine);

    me::orchestrator::Exporter exporter(engine, borrow_timeline(timeline));
    std::unique_ptr<me::orchestrator::Exporter::Job> job;
    std::string err;
    me_status_t s = exporter.export_to(*output, cb, user, &job, &err);
    if (s != ME_OK) {
        me::detail::set_error(engine, std::move(err));
        return s;
    }

    auto* wrapper = new me_render_job{};
    wrapper->engine = engine;
    wrapper->job    = std::move(job);
    *out_job = wrapper;
    return ME_OK;
}

extern "C" me_status_t me_render_cancel(me_render_job_t* job) {
    if (!job || !job->job) return ME_E_INVALID_ARG;
    job->job->cancel.store(true, std::memory_order_release);
    return ME_OK;
}

extern "C" me_status_t me_render_wait(me_render_job_t* job) {
    if (!job || !job->job) return ME_E_INVALID_ARG;
    if (job->job->worker.joinable()) job->job->worker.join();
    /* Join is the synchronization point for the worker-thread-populated
     * err_msg. Propagate into the caller's thread-local slot so
     * me_engine_last_error on this thread reflects the async failure. */
    if (job->job->result != ME_OK && !job->job->err_msg.empty() && job->engine) {
        me::detail::set_error(job->engine, job->job->err_msg);
    }
    return job->job->result;
}

extern "C" void me_render_job_destroy(me_render_job_t* job) {
    if (!job) return;
    if (job->job && job->job->worker.joinable()) job->job->worker.join();
    delete job;
}

/* --- Frame server: compose path (a) + disk_cache + me_frame wrap --------- */

extern "C" me_status_t me_render_frame(
    me_engine_t*         engine,
    const me_timeline_t* timeline,
    me_rational_t        time,
    me_frame_t**         out_frame) {

    if (!engine || !timeline || !out_frame) return ME_E_INVALID_ARG;
    if (!engine->scheduler) return ME_E_INVALID_ARG;
    *out_frame = nullptr;
    me::detail::clear_error(engine);

    /* Compile the per-frame compose graph first so cache lookup can
     * key off graph.content_hash(). compile_compose_graph returns
     * ME_E_NOT_FOUND when `time` falls in a gap (no contributing
     * video layer). */
    me::graph::Graph   graph;
    me::graph::PortRef terminal{};
    if (const auto s = me::orchestrator::compile_compose_graph(
            timeline->tl, time, &graph, &terminal); s != ME_OK) {
        return s;
    }

    /* DiskCache peek — keyed by the compiled graph's content hash so
     * same (timeline-state, time) hits, distinct timelines or distinct
     * times miss. This replaces the legacy
     * `<asset_hash>:<source_num>:<source_den>` key shape that
     * single-track timelines used. */
    const std::string cache_key = disk_cache_key_from(graph);
    if (engine->disk_cache && engine->disk_cache->enabled()) {
        auto cached = engine->disk_cache->get(cache_key);
        if (cached.has_value()) {
            auto mf = std::make_unique<me_frame>();
            mf->width  = cached->width;
            mf->height = cached->height;
            mf->stride = cached->stride;
            mf->rgba   = std::move(cached->rgba);
            *out_frame = mf.release();
            return ME_OK;
        }
    }

    /* Cache miss — submit + await the per-frame video graph. Inline
     * the evaluate (rather than calling compose_frame_at) so the
     * graph object built above flows directly into the scheduler;
     * compose_frame_at would re-build it. */
    std::shared_ptr<me::graph::RgbaFrameData> rgba;
    me::graph::EvalContext ctx;
    ctx.frames = engine->frames.get();
    ctx.codecs = engine->codecs.get();
    ctx.time   = time;
    try {
        auto fut = engine->scheduler->evaluate_port<
                       std::shared_ptr<me::graph::RgbaFrameData>>(graph, terminal, ctx);
        rgba = fut.await();
    } catch (const std::exception& ex) {
        me::detail::set_error(engine, std::string{ex.what()});
        return ME_E_DECODE;
    }
    if (!rgba) return ME_E_DECODE;

    auto mf = std::make_unique<me_frame>();
    mf->width  = rgba->width;
    mf->height = rgba->height;
    mf->stride = static_cast<int>(rgba->stride);
    /* Copy bytes: the OutputCache shared_ptr may alias these pixels;
     * the consumer owns + may free out-of-order. The extra copy on
     * cache-hit is the price of pure-data RgbaFrameData. */
    mf->rgba = rgba->rgba;

    /* DiskCache write-through (persistent layer). Best-effort —
     * failures don't affect the render path. */
    if (engine->disk_cache && engine->disk_cache->enabled()) {
        engine->disk_cache->put(cache_key, mf->rgba.data(),
                                 mf->width, mf->height,
                                 static_cast<int>(mf->stride));
    }

    *out_frame = mf.release();
    return ME_OK;
}

extern "C" void me_frame_destroy(me_frame_t* frame) {
    delete frame;
}
extern "C" int me_frame_width(const me_frame_t* f) {
    return f ? f->width : 0;
}
extern "C" int me_frame_height(const me_frame_t* f) {
    return f ? f->height : 0;
}
extern "C" const uint8_t* me_frame_pixels(const me_frame_t* f) {
    return (f && !f->rgba.empty()) ? f->rgba.data() : nullptr;
}
