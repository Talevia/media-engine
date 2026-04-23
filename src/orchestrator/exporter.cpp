#include "orchestrator/exporter.hpp"

#include "core/engine_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "io/demux_context.hpp"
#include "orchestrator/output_sink.hpp"
#include "scheduler/scheduler.hpp"
#include "task/task_kind.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace me::orchestrator {

namespace {

/* Build a single-demux-node graph for one clip URI. */
std::pair<std::shared_ptr<graph::Graph>, graph::PortRef>
build_demux_graph(const std::string& uri) {
    graph::Graph::Builder b;
    graph::Properties props;
    props["uri"].v = uri;
    graph::NodeId n = b.add(task::TaskKindId::IoDemux, std::move(props), {});
    graph::PortRef terminal{n, 0};
    b.name_terminal("demux", terminal);
    auto g = std::make_shared<graph::Graph>(std::move(b).build());
    return {g, terminal};
}

struct ClipPlan {
    std::shared_ptr<graph::Graph> graph;
    graph::PortRef                term;
    ClipTimeRange                 range;
};

/* Resolve asset_id → uri via the Timeline's asset table. Used by the
 * per-clip demux graph builder; loader has already rejected unknown ids,
 * so `at()` is safe here (missing id is a programmer error, not a
 * runtime-recoverable one). */
const std::string& resolve_uri(const me::Timeline& tl, const std::string& asset_id) {
    return tl.assets.at(asset_id).uri;
}

}  // namespace

me_status_t Exporter::export_to(const me_output_spec_t& spec,
                                 me_progress_cb         cb,
                                 void*                  user,
                                 std::unique_ptr<Job>*  out_job,
                                 std::string*           err) {
    if (out_job) *out_job = nullptr;

    if (!tl_ || tl_->clips.empty()) {
        if (err) *err = "phase-1: timeline must have at least one clip";
        return ME_E_UNSUPPORTED;
    }

    /* Compile a demux graph + carry a ClipTimeRange per clip. */
    std::vector<ClipPlan> plans;
    plans.reserve(tl_->clips.size());
    for (const auto& clip : tl_->clips) {
        const std::string& uri = resolve_uri(*tl_, clip.asset_id);
        auto [g, term] = build_demux_graph(uri);
        graph_cache_.insert(g->content_hash(), g);
        plans.push_back(ClipPlan{
            std::move(g), term,
            ClipTimeRange{
                clip.source_start,
                clip.time_duration,   /* phase-1: source_dur == time_dur */
                clip.time_start,
            },
        });
    }

    auto job = std::make_unique<Job>();
    job->output_path = spec.path ? spec.path : "";

    /* Build the sink up front so spec errors surface synchronously before
     * the worker thread spawns. Progress callback + cancel flag bind into
     * the sink at construction. */
    auto progress_cb = [cb, user](float r) {
        if (!cb) return;
        me_progress_event_t ev{};
        ev.kind  = ME_PROGRESS_FRAMES;
        ev.ratio = r;
        cb(&ev, user);
    };
    SinkCommon common;
    common.out_path  = job->output_path;
    common.container = spec.container ? spec.container : "";
    common.cancel    = &job->cancel;
    if (cb) common.on_ratio = progress_cb;

    std::vector<ClipTimeRange> ranges;
    ranges.reserve(plans.size());
    for (const auto& p : plans) ranges.push_back(p.range);

    std::unique_ptr<OutputSink> sink = make_output_sink(
        spec, std::move(common), std::move(ranges),
        engine_ ? engine_->codecs.get() : nullptr, err);
    if (!sink) return ME_E_UNSUPPORTED;

    me_engine* eng = engine_;
    Job* raw = job.get();

    /* Capture-list hygiene: the sink owns all spec-derived state so the
     * worker doesn't need to capture spec fields separately. */
    auto sink_shared = std::shared_ptr<OutputSink>(sink.release());

    raw->worker = std::thread([eng, cb, user, raw, plans, sink_shared]() {
        if (cb) {
            me_progress_event_t ev{};
            ev.kind = ME_PROGRESS_STARTED;
            cb(&ev, user);
        }

        graph::EvalContext ctx;
        me_status_t final_status = ME_OK;
        std::string work_err;

        /* Drive one demux graph per clip, collecting DemuxContext handles.
         * Each DemuxContext stays alive until sink->process returns. */
        std::vector<std::shared_ptr<io::DemuxContext>> demuxes;
        demuxes.reserve(plans.size());
        for (size_t i = 0; i < plans.size() && final_status == ME_OK; ++i) {
            auto fut = eng->scheduler->evaluate_port<std::shared_ptr<io::DemuxContext>>(
                           *plans[i].graph, plans[i].term, ctx);
            try {
                demuxes.push_back(fut.await());
            } catch (const std::exception& ex) {
                final_status = ME_E_IO;
                work_err = "demux[" + std::to_string(i) + "]: " + ex.what();
            }
        }

        if (final_status == ME_OK) {
            final_status = sink_shared->process(std::move(demuxes), &work_err);
        }

        raw->result = final_status;
        if (final_status != ME_OK) {
            /* Stash on the Job; caller's thread-local last-error slot is
             * populated in me_render_wait after the worker joins. */
            raw->err_msg = std::move(work_err);
        }
        (void)eng;  /* kept in capture list for future per-engine state */

        if (cb) {
            me_progress_event_t ev{};
            if (final_status == ME_OK) {
                ev.kind        = ME_PROGRESS_COMPLETED;
                ev.output_path = raw->output_path.c_str();
            } else {
                ev.kind   = ME_PROGRESS_FAILED;
                ev.status = final_status;
            }
            cb(&ev, user);
        }
    });

    *out_job = std::move(job);
    return ME_OK;
}

}  // namespace me::orchestrator
