#include "media_engine/render.h"
#include "core/engine_impl.hpp"
#include "core/frame_impl.hpp"
#include "graph/types.hpp"
#include "orchestrator/compose_frame.hpp"
#include "orchestrator/exporter.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/disk_cache.hpp"
#include "timeline/timeline_impl.hpp"

#include <memory>
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
    *out_frame = nullptr;
    me::detail::clear_error(engine);

    /* Resolve which clip is active at `time`; if `time` falls in a
     * gap (or past end) report NOT_FOUND to the host. */
    me::orchestrator::ResolvedClip clip;
    if (const auto s = me::orchestrator::resolve_active_clip_at(
            timeline->tl, time, &clip); s != ME_OK) {
        return s;
    }

    /* DiskCache peek (persistent across processes). Key =
     * `<asset_hash>:<source_num>:<source_den>` so the same (asset,
     * timeline-local moment) returns the cached frame and all entries
     * for a given asset share a common prefix
     * (me_cache_invalidate_asset walks on prefix). The in-process
     * scheduler.cache (inside the engine's scheduler) handles same-
     * process repeat fetches; disk_cache survives process restarts
     * and is what me_cache_stats / me_cache_clear /
     * me_cache_invalidate_asset (src/api/cache.cpp) report on. Empty
     * asset_hash → skip cache (can't key). */
    std::string asset_hash;
    if (engine->asset_hashes) {
        asset_hash = engine->asset_hashes->get_or_compute(clip.uri);
    }
    std::string cache_key;
    if (!asset_hash.empty()) {
        cache_key = asset_hash + ":" +
                    std::to_string(clip.source_t.num) + ":" +
                    std::to_string(clip.source_t.den);
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
    }

    /* Cache miss — submit + await the per-frame video graph. */
    std::shared_ptr<me::graph::RgbaFrameData> rgba;
    std::string err;
    if (const auto s = me::orchestrator::compose_frame_at(
            engine, timeline->tl, time, &rgba, &err); s != ME_OK) {
        if (!err.empty()) me::detail::set_error(engine, std::move(err));
        return s;
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
    if (!cache_key.empty() &&
        engine->disk_cache && engine->disk_cache->enabled()) {
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
