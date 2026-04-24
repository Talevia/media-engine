/*
 * Exporter — batch encoding: timeline + output_spec → file on disk.
 *
 * Routing (see exporter.cpp::export_to):
 *   - audio-only timeline → make_audio_only_sink
 *   - multi-track OR has transitions OR has audio tracks →
 *     make_compose_sink (runs the per-frame compose loop + audio
 *     mixer path; requires h264+aac encoder combo today).
 *   - otherwise → make_output_sink (single-track passthrough /
 *     h264+aac reencode via reencode_pipeline).
 *
 * Supported codec combos: `passthrough` (stream-copy) and
 * `h264` + `aac` (h264_videotoolbox on macOS; other platforms
 * skip with ME_E_UNSUPPORTED per hardware availability). Unknown
 * codec names return ME_E_UNSUPPORTED synchronously; sink-layer
 * mismatches (e.g. compose path with passthrough) are rejected
 * at render_start time.
 */
#pragma once

#include "media_engine/render.h"
#include "media_engine/types.h"
#include "orchestrator/segment_cache.hpp"
#include "timeline/timeline_impl.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

struct me_engine;

namespace me::orchestrator {

class Exporter {
public:
    Exporter(me_engine* engine, std::shared_ptr<const Timeline> timeline)
        : engine_(engine), tl_(std::move(timeline)) {}

    /* Active in-flight export. Opaque to callers — wraps the worker thread
     * + cancel flag + terminal status. The me_render_job_t C handle wraps
     * this one-to-one. */
    struct Job {
        std::thread                worker;
        std::atomic<bool>          cancel{false};
        me_status_t                result{ME_OK};
        std::string                output_path;

        /* Error message captured on the worker thread. The caller's
         * thread-local last-error slot is populated from here by
         * me_render_wait after the worker joins (see api/render.cpp) —
         * thread_local storage is per-thread, so the worker cannot
         * write directly into the caller's slot. */
        std::string                err_msg;
    };

    /* Start an export. Spawns a worker thread; progress events fire on that
     * thread. Returns ownership of the Job to the caller, who joins via
     * wait()/destroy(). Returns ME_OK on submission (failures happen
     * asynchronously); synchronous validation failures (bad spec / unknown
     * codec) return non-OK immediately and *out_job stays null. */
    me_status_t export_to(const me_output_spec_t& spec,
                          me_progress_cb          cb,
                          void*                   user,
                          std::unique_ptr<Job>*   out_job,
                          std::string*            err);

private:
    me_engine*                       engine_;
    std::shared_ptr<const Timeline>  tl_;
    SegmentCache                     graph_cache_;  /* populated once graph migration lands */
};

}  // namespace me::orchestrator
