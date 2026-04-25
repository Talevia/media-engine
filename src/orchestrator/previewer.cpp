#include "orchestrator/previewer.hpp"

#include "core/engine_impl.hpp"
#include "core/frame_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "orchestrator/previewer_graph.hpp"
#include "resource/asset_hash_cache.hpp"
#include "resource/disk_cache.hpp"
#include "scheduler/scheduler.hpp"
#include "timeline/timeline_impl.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace me::orchestrator {

me_status_t Previewer::frame_at(me_rational_t time, me_frame** out_frame) {
    if (!out_frame) return ME_E_INVALID_ARG;
    *out_frame = nullptr;
    if (!tl_) return ME_E_INVALID_ARG;
    if (tl_->tracks.empty() || tl_->clips.empty()) return ME_E_INVALID_ARG;

    /* Normalize input time. */
    if (time.den <= 0) time.den = 1;
    if (time.num < 0)  time.num = 0;

    /* Find the bottom track's active clip at t. Phase-1 single-
     * track frame server — multi-track compose via the full
     * compose_decode_loop path is a follow-up cycle
     * (previewer-multi-track-compose-graph). */
    const std::string& bottom_id = tl_->tracks[0].id;
    const me::Clip*   active = nullptr;
    me_rational_t     clip_local_t{0, 1};

    for (const auto& c : tl_->clips) {
        if (c.track_id != bottom_id) continue;

        /* time_start + time_duration in rational form: sum. */
        const int64_t e_num = c.time_start.num * c.time_duration.den +
                              c.time_duration.num * c.time_start.den;
        const int64_t e_den = c.time_start.den * c.time_duration.den;

        /* t >= start AND t < end, cross-multiplied. */
        const bool ge_start =
            time.num * c.time_start.den >= c.time_start.num * time.den;
        const bool lt_end =
            time.num * e_den < e_num * time.den;
        if (!ge_start || !lt_end) continue;

        active = &c;
        /* source_t = source_start + (t - time_start). The "t -
         * time_start" part is rational subtract: a/b - c/d =
         * (ad - cb)/(bd). */
        clip_local_t = me_rational_t{
            time.num * c.time_start.den - c.time_start.num * time.den,
            time.den * c.time_start.den,
        };
        break;
    }
    if (!active) return ME_E_NOT_FOUND;

    /* source_t_abs = source_start + clip_local_t. */
    const me_rational_t source_t{
        active->source_start.num * clip_local_t.den +
            clip_local_t.num * active->source_start.den,
        active->source_start.den * clip_local_t.den,
    };

    /* Asset lookup. */
    auto a_it = tl_->assets.find(active->asset_id);
    if (a_it == tl_->assets.end()) return ME_E_NOT_FOUND;
    const std::string& uri = a_it->second.uri;

    /* DiskCache peek (persistent across processes). Key =
     * `<asset_hash>:<source_num>:<source_den>` so the same (asset,
     * timeline-local moment) returns the cached frame and all entries
     * for a given asset share a common prefix
     * (me_cache_invalidate_asset walks on prefix). The in-process
     * scheduler.cache (phase 2) handles same-process repeat fetches;
     * disk_cache survives process restarts and is what
     * me_cache_stats / me_cache_clear / me_cache_invalidate_asset
     * (src/api/cache.cpp) report on. Both layers are kept (D3 in the
     * implementation plan). Empty asset_hash → skip cache (can't key). */
    std::string asset_hash;
    if (engine_ && engine_->asset_hashes) {
        asset_hash = engine_->asset_hashes->get_or_compute(uri);
    }
    std::string cache_key;
    if (!asset_hash.empty()) {
        cache_key = asset_hash + ":" +
                    std::to_string(source_t.num) + ":" +
                    std::to_string(source_t.den);
        if (engine_ && engine_->disk_cache && engine_->disk_cache->enabled()) {
            auto cached = engine_->disk_cache->get(cache_key);
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

    /* Build + evaluate the per-frame decode graph. The scheduler's
     * OutputCache handles in-process repeat fetches; on miss the
     * graph runs end-to-end (open → decode → sws_scale → RGBA8). */
    if (!engine_ || !engine_->scheduler) return ME_E_INTERNAL;

    auto [g, term] = compile_frame_graph(uri, source_t);

    graph::EvalContext ctx;
    ctx.frames = engine_->frames.get();
    ctx.codecs = engine_->codecs.get();
    ctx.time   = source_t;   /* time_invariant kernels ignore; cache mixer uses for non-invariant nodes */

    std::shared_ptr<graph::RgbaFrameData> rgba;
    try {
        auto fut = engine_->scheduler->evaluate_port<
                       std::shared_ptr<graph::RgbaFrameData>>(g, term, ctx);
        rgba = fut.await();
    } catch (const std::exception& ex) {
        me::detail::set_error(engine_, ex.what());
        return ME_E_DECODE;
    }
    if (!rgba) return ME_E_DECODE;

    /* Wrap into the public me_frame. We move the bytes out of the
     * cache value so the consumer owns them; subsequent OutputCache
     * hits will copy via shared_ptr's clone-on-modify pattern (since
     * the cache stored a shared_ptr that may be aliased — see note). */
    auto mf = std::make_unique<me_frame>();
    mf->width  = rgba->width;
    mf->height = rgba->height;
    mf->stride = rgba->stride;
    /* shared_ptr inside the cache may still alias these bytes; copy
     * to be safe (the consumer owns + may free out-of-order). The
     * extra copy on the cache-hit path is the price of pure-data
     * RgbaFrameData; a future bullet can switch to a refcounted
     * me_frame if scrubbing pressure shows up. */
    mf->rgba = rgba->rgba;

    /* DiskCache write-through (persistent layer). Best-effort —
     * failures don't affect the render path. */
    if (!cache_key.empty() &&
        engine_ && engine_->disk_cache && engine_->disk_cache->enabled()) {
        engine_->disk_cache->put(cache_key, mf->rgba.data(),
                                  mf->width, mf->height,
                                  static_cast<int>(mf->stride));
    }

    *out_frame = mf.release();
    return ME_OK;
}

}  // namespace me::orchestrator
