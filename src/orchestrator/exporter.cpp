#include "orchestrator/exporter.hpp"

#include "core/engine_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "io/demux_context.hpp"
#include "orchestrator/muxer_state.hpp"
#include "orchestrator/reencode_pipeline.hpp"
#include "scheduler/scheduler.hpp"
#include "task/task_kind.hpp"

#include <memory>
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

bool is_h264_aac_spec(const me_output_spec_t& s) {
    return streq(s.video_codec, "h264") && streq(s.audio_codec, "aac");
}

/* Build a trivial Graph with a single io::demux node whose "source" output
 * is named as the "demux" terminal. Returns the compiled Graph + the
 * terminal PortRef. */
std::pair<std::shared_ptr<graph::Graph>, graph::PortRef>
build_passthrough_graph(const std::string& uri) {
    graph::Graph::Builder b;
    graph::Properties props;
    props["uri"].v = uri;
    graph::NodeId n = b.add(task::TaskKindId::IoDemux, std::move(props), {});
    graph::PortRef terminal{n, 0};
    b.name_terminal("demux", terminal);
    auto g = std::make_shared<graph::Graph>(std::move(b).build());
    return {g, terminal};
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
    const bool passthrough = is_passthrough_spec(spec);
    const bool reencode    = !passthrough && is_h264_aac_spec(spec);
    if (!passthrough && !reencode) {
        if (err) *err = "phase-1: supported specs are "
                         "(video=passthrough, audio=passthrough) or (video=h264, audio=aac)";
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
    const std::string vcodec    = spec.video_codec ? spec.video_codec : "";
    const std::string acodec    = spec.audio_codec ? spec.audio_codec : "";
    const int64_t     vbr       = spec.video_bitrate_bps;
    const int64_t     abr       = spec.audio_bitrate_bps;

    me_engine* eng = engine_;
    Job* raw = job.get();

    /* Compile & cache the per-segment graph. Passthrough has a single
     * trivial segment (single clip, no transitions), so the cache's
     * boundary_hash keying is exercised end-to-end without being
     * stressful yet. */
    auto [g, terminal] = build_passthrough_graph(in_uri);
    graph_cache_.insert(g->content_hash(), g);
    graph::PortRef term = terminal;  /* capture-safe copy */

    raw->worker = std::thread([eng, cb, user, raw, g, term,
                               out_path, container, passthrough,
                               vcodec, acodec, vbr, abr]() {
        if (cb) {
            me_progress_event_t ev{};
            ev.kind = ME_PROGRESS_STARTED;
            cb(&ev, user);
        }

        /* Drive the graph: a single io::demux node whose output is a
         * shared_ptr<io::DemuxContext>. Scheduler picks the kernel by
         * Affinity hint (Cpu at bootstrap since we don't have an Io pool
         * yet; io::demux registered with Affinity::Io so the lookup
         * falls through to primary registration). */
        graph::EvalContext ctx;
        /* FramePool/CodecPool/GpuCtx live on the engine; bootstrap doesn't
         * need to thread them explicitly — scheduler gets them via its
         * own refs passed at construction. */
        auto fut = eng->scheduler->evaluate_port<std::shared_ptr<io::DemuxContext>>(
                       *g, term, ctx);

        me_status_t final_status = ME_OK;
        std::string work_err;
        std::shared_ptr<io::DemuxContext> demux;

        try {
            demux = fut.await();
        } catch (const std::exception& ex) {
            final_status = ME_E_IO;
            work_err     = std::string("demux: ") + ex.what();
        }

        if (final_status == ME_OK) {
            auto progress_cb = [cb, user](float r) {
                if (!cb) return;
                me_progress_event_t ev{};
                ev.kind  = ME_PROGRESS_FRAMES;
                ev.ratio = r;
                cb(&ev, user);
            };

            if (passthrough) {
                PassthroughMuxOptions opts;
                opts.out_path  = out_path;
                opts.container = container;
                opts.cancel    = &raw->cancel;
                if (cb) opts.on_ratio = progress_cb;
                final_status = passthrough_mux(*demux, opts, &work_err);
            } else {
                ReencodeOptions opts;
                opts.out_path           = out_path;
                opts.container          = container;
                opts.video_codec        = vcodec;
                opts.audio_codec        = acodec;
                opts.video_bitrate_bps  = vbr;
                opts.audio_bitrate_bps  = abr;
                opts.cancel             = &raw->cancel;
                if (cb) opts.on_ratio   = progress_cb;
                final_status = reencode_mux(*demux, opts, &work_err);
            }
        }

        raw->result = final_status;
        if (final_status != ME_OK) {
            me::detail::set_error(eng, work_err);
        }

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
