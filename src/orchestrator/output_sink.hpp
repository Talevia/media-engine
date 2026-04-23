/*
 * OutputSink — the abstraction over "what does the exporter produce?"
 *
 * Previously `Exporter::export_to` dispatched to either `passthrough_mux`
 * or `reencode_mux` via an inline if-else on `me_output_spec_t` fields.
 * Adding a third output (ProRes, HEVC, Opus-only audio, ...) meant
 * growing the if-else, capturing yet more lambda state, and replicating
 * "drive demux → hand to mux" plumbing per branch. The sink abstraction
 * inverts that: the Exporter classifies the spec once via a factory, and
 * the worker thread invokes a single `sink->process(demuxes, err)` call.
 *
 * Each sink captures its encoder/mux parameters at construction; the
 * common path + container + cancel + progress state lives in
 * `SinkCommon`. Per-clip time ranges ride alongside via a parallel
 * `std::vector<ClipTimeRange>`; the factory zips them with the supplied
 * demuxes at process() time.
 */
#pragma once

#include "media_engine/render.h"
#include "media_engine/types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace me::io { class DemuxContext; }

namespace me::orchestrator {

/* Parameters shared across every sink kind: the output file, the cancel
 * flag monitored cooperatively per-packet, and the progress callback. */
struct SinkCommon {
    std::string                out_path;
    std::string                container;          /* empty = infer */
    std::function<void(float)> on_ratio;
    const std::atomic<bool>*   cancel = nullptr;
};

/* Per-clip time window. Paired positionally with the DemuxContexts that
 * `process()` receives — both are indexed by clip position. */
struct ClipTimeRange {
    me_rational_t source_start    { 0, 1 };
    me_rational_t source_duration { 0, 1 };
    me_rational_t time_offset     { 0, 1 };
};

class OutputSink {
public:
    virtual ~OutputSink() = default;

    /* Consume the opened demuxes (positionally aligned with the clip time
     * ranges the sink was built with) and produce the output file. */
    virtual me_status_t process(
        std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes,
        std::string*                                       err) = 0;
};

/* Factory: classify the spec, build the matching sink. Returns nullptr
 * and writes a diagnostic to *err on unsupported specs. The factory is
 * also responsible for enforcing per-sink clip-count constraints (e.g.
 * the h264/aac sink is single-clip only in phase-1) — a spec that
 * can't be served with this clip count returns nullptr. */
std::unique_ptr<OutputSink> make_output_sink(
    const me_output_spec_t&            spec,
    SinkCommon                         common,
    std::vector<ClipTimeRange>         clip_ranges,
    std::string*                       err);

}  // namespace me::orchestrator
