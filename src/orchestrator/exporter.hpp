/*
 * Exporter — batch encoding: timeline + output_spec → file on disk.
 *
 * Bootstrap: the passthrough path still runs through the legacy
 * io::remux_passthrough function. The refactor-passthrough-into-graph-
 * exporter backlog item replaces the internals with an io::demux kernel
 * + MuxerState driven by the scheduler.
 *
 * Non-passthrough codecs return ME_E_UNSUPPORTED for now.
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
