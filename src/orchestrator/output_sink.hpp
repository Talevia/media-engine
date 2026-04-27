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
#include "timeline/timeline_impl.hpp"   /* me::ColorSpace */

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace me::io       { class DemuxContext; }
namespace me::resource { class CodecPool; }

namespace me::orchestrator {

/* Parameters shared across every sink kind: the output file, the cancel
 * flag monitored cooperatively per-packet, and the progress callback. */
struct SinkCommon {
    std::string                out_path;
    std::string                container;          /* empty = infer */
    std::function<void(float)> on_ratio;
    const std::atomic<bool>*   cancel = nullptr;
    /* Timeline-level target / working color space (from
     * `me::Timeline::color_space`). Sink passes it down to the
     * `me::color::Pipeline::apply()` call in the reencode path.
     * Default-constructed ColorSpace means UNSPECIFIED (IdentityPipeline
     * ignores; OcioPipeline treats as pass-through). */
    me::ColorSpace             target_color_space {};

    /* Engine-level OCIO config path override
     * (`me_engine_config_t.ocio_config_path`). Empty = inherit
     * from `$OCIO` env var or fall through to the built-in ACES
     * CG config. Threaded through to `make_pipeline()` at
     * encoder-mux setup time (encoder_mux_setup.cpp). The
     * exporter copies the engine config's `const char*` into a
     * `std::string` here so the worker thread sees a stable
     * value regardless of the host's lifetime for the original
     * pointer. */
    std::string                ocio_config_path;
};

/* Per-clip time window. Paired positionally with the DemuxContexts that
 * `process()` receives — both are indexed by clip position. */
struct ClipTimeRange {
    me_rational_t source_start    { 0, 1 };
    me_rational_t source_duration { 0, 1 };
    me_rational_t time_offset     { 0, 1 };
    /* Per-clip source color space, from the Asset the clip references
     * (`me::Asset::color_space` via `resolve_uri`-like lookup inside
     * the Exporter). Default = UNSPECIFIED. */
    me::ColorSpace source_color_space {};
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
    me::resource::CodecPool*           codec_pool,   /* required for re-encode sinks */
    std::string*                       err);

}  // namespace me::orchestrator
