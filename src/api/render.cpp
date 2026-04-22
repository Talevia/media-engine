#include "media_engine/render.h"
#include "core/engine_impl.hpp"
#include "timeline/timeline_impl.hpp"
#include "io/ffmpeg_remux.hpp"

#include <atomic>
#include <string>
#include <thread>

struct me_render_job {
    std::thread              worker;
    std::atomic<bool>        cancel{false};
    std::atomic<bool>        finished{false};
    me_status_t              result{ME_OK};
    std::string              output_path;
};

namespace {

bool is_passthrough(const me_output_spec_t* out) {
    auto streq = [](const char* a, const char* b) {
        return a && b && std::string(a) == b;
    };
    return streq(out->video_codec, "passthrough") &&
           streq(out->audio_codec, "passthrough");
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

    if (!is_passthrough(output)) {
        me::detail::set_error(engine,
            "phase-1: only video_codec=\"passthrough\" + audio_codec=\"passthrough\" supported");
        return ME_E_UNSUPPORTED;
    }
    if (timeline->tl.clips.size() != 1) {
        me::detail::set_error(engine, "phase-1: timeline must have exactly one clip");
        return ME_E_UNSUPPORTED;
    }
    if (!output->path) {
        me::detail::set_error(engine, "output.path is required");
        return ME_E_INVALID_ARG;
    }

    auto* job = new me_render_job{};
    job->output_path = output->path;

    const std::string in_uri        = timeline->tl.clips[0].asset_uri;
    const std::string out_path      = output->path;
    const std::string container     = output->container ? output->container : "";

    job->worker = std::thread([engine, cb, user, job, in_uri, out_path, container]() {
        if (cb) {
            me_progress_event_t ev{};
            ev.kind = ME_PROGRESS_STARTED;
            cb(&ev, user);
        }

        std::string err;
        auto on_ratio = [&](float r) {
            if (!cb) return;
            me_progress_event_t ev{};
            ev.kind  = ME_PROGRESS_FRAMES;
            ev.ratio = r;
            cb(&ev, user);
        };

        me_status_t s = me::io::remux_passthrough(
            in_uri, out_path, container, on_ratio, job->cancel, &err);

        job->result = s;
        job->finished.store(true, std::memory_order_release);

        if (s != ME_OK) {
            me::detail::set_error(engine, err);
        }

        if (cb) {
            me_progress_event_t ev{};
            if (s == ME_OK) {
                ev.kind        = ME_PROGRESS_COMPLETED;
                ev.output_path = job->output_path.c_str();
            } else {
                ev.kind   = ME_PROGRESS_FAILED;
                ev.status = s;
            }
            cb(&ev, user);
        }
    });

    *out_job = job;
    return ME_OK;
}

extern "C" me_status_t me_render_cancel(me_render_job_t* job) {
    if (!job) return ME_E_INVALID_ARG;
    job->cancel.store(true, std::memory_order_release);
    return ME_OK;
}

extern "C" me_status_t me_render_wait(me_render_job_t* job) {
    if (!job) return ME_E_INVALID_ARG;
    if (job->worker.joinable()) job->worker.join();
    return job->result;
}

extern "C" void me_render_job_destroy(me_render_job_t* job) {
    if (!job) return;
    if (job->worker.joinable()) job->worker.join();
    delete job;
}

/* --- Frame server: not implemented in phase 1 ---------------------------- */

extern "C" me_status_t me_render_frame(
    me_engine_t*, const me_timeline_t*, me_rational_t, me_frame_t** out_frame) {
    if (out_frame) *out_frame = nullptr;
    return ME_E_UNSUPPORTED;
}

extern "C" void           me_frame_destroy(me_frame_t*)     {}
extern "C" int            me_frame_width(const me_frame_t*) { return 0; }
extern "C" int            me_frame_height(const me_frame_t*){ return 0; }
extern "C" const uint8_t* me_frame_pixels(const me_frame_t*){ return nullptr; }
