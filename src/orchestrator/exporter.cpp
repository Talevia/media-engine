#include "orchestrator/exporter.hpp"

#include "core/engine_impl.hpp"
#include "io/ffmpeg_remux.hpp"

#include <string>
#include <utility>

namespace me::orchestrator {

namespace {

bool streq(const char* a, const char* b) {
    return a && b && std::string(a) == b;
}

bool is_passthrough_spec(const me_output_spec_t& s) {
    return streq(s.video_codec, "passthrough") && streq(s.audio_codec, "passthrough");
}

}  // namespace

me_status_t Exporter::export_to(const me_output_spec_t& spec,
                                 me_progress_cb         cb,
                                 void*                  user,
                                 std::unique_ptr<Job>*  out_job,
                                 std::string*           err) {
    if (out_job) *out_job = nullptr;

    if (!spec.path) {
        if (err) *err = "output.path is required";
        return ME_E_INVALID_ARG;
    }
    if (!is_passthrough_spec(spec)) {
        if (err) *err = "phase-1: only video_codec=\"passthrough\" + audio_codec=\"passthrough\" supported";
        return ME_E_UNSUPPORTED;
    }
    if (!tl_ || tl_->clips.size() != 1) {
        if (err) *err = "phase-1: timeline must have exactly one clip";
        return ME_E_UNSUPPORTED;
    }

    auto job = std::make_unique<Job>();
    job->output_path = spec.path;

    const std::string in_uri    = tl_->clips[0].asset_uri;
    const std::string out_path  = spec.path;
    const std::string container = spec.container ? spec.container : "";

    me_engine* eng = engine_;
    Job* raw = job.get();

    raw->worker = std::thread([eng, cb, user, raw, in_uri, out_path, container]() {
        if (cb) {
            me_progress_event_t ev{};
            ev.kind = ME_PROGRESS_STARTED;
            cb(&ev, user);
        }

        std::string work_err;
        auto on_ratio = [&](float r) {
            if (!cb) return;
            me_progress_event_t ev{};
            ev.kind  = ME_PROGRESS_FRAMES;
            ev.ratio = r;
            cb(&ev, user);
        };

        me_status_t s = io::remux_passthrough(
            in_uri, out_path, container, on_ratio, raw->cancel, &work_err);

        raw->result = s;
        if (s != ME_OK) {
            me::detail::set_error(eng, work_err);
        }

        if (cb) {
            me_progress_event_t ev{};
            if (s == ME_OK) {
                ev.kind        = ME_PROGRESS_COMPLETED;
                ev.output_path = raw->output_path.c_str();
            } else {
                ev.kind   = ME_PROGRESS_FAILED;
                ev.status = s;
            }
            cb(&ev, user);
        }
    });

    *out_job = std::move(job);
    return ME_OK;
}

}  // namespace me::orchestrator
