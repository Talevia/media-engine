#include "media_engine/render.h"
#include "core/engine_impl.hpp"
#include "orchestrator/exporter.hpp"
#include "orchestrator/previewer.hpp"
#include "timeline/timeline_impl.hpp"

#include <memory>
#include <string>

/* me_render_job wraps the orchestrator's opaque Job — the C API shape stays
 * stable even as the orchestrator evolves. */
struct me_render_job {
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
    wrapper->job = std::move(job);
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
    return job->job->result;
}

extern "C" void me_render_job_destroy(me_render_job_t* job) {
    if (!job) return;
    if (job->job && job->job->worker.joinable()) job->job->worker.join();
    delete job;
}

/* --- Frame server: delegates to Previewer, which stubs until M6 --------- */

extern "C" me_status_t me_render_frame(
    me_engine_t*         engine,
    const me_timeline_t* timeline,
    me_rational_t        time,
    me_frame_t**         out_frame) {

    if (!engine || !timeline || !out_frame) return ME_E_INVALID_ARG;
    me::detail::clear_error(engine);

    me::orchestrator::Previewer previewer(engine, borrow_timeline(timeline));
    return previewer.frame_at(time, out_frame);
}

/* STUB: frame-server-impl — me_frame_* accessors; land with the M6 frame server. */
extern "C" void           me_frame_destroy(me_frame_t*)     {}
extern "C" int            me_frame_width(const me_frame_t*) { return 0; }
extern "C" int            me_frame_height(const me_frame_t*){ return 0; }
extern "C" const uint8_t* me_frame_pixels(const me_frame_t*){ return nullptr; }
