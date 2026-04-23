/*
 * ComposeSink — OutputSink variant for multi-track video composition.
 *
 * Opened as the architectural home for the per-frame multi-track
 * compose loop. The four prerequisites (schema/IR, alpha_over kernel,
 * active_clips resolver, YUV↔RGBA frame_convert) are in place; this
 * class is where they get glued into a render-path OutputSink that
 * the encoder/mux consume.
 *
 * This first landing only establishes the class + factory + Exporter
 * routing. `process()` still returns ME_E_UNSUPPORTED — the behavior
 * change from the previous Exporter-layer gate is that the rejection
 * now surfaces **asynchronously** from `me_render_wait` (worker
 * thread) rather than synchronously from `me_render_start`. This
 * matches the runtime path that subsequent cycles need (encoder +
 * mux setup will only work once per-frame compose is in place, and
 * both live inside `process()`).
 *
 * The follow-up `multi-track-compose-frame-loop` backlog bullet
 * replaces the UNSUPPORTED stub with the actual compose loop:
 *   active_clips_at(tl, T) → decode each track → frame_to_rgba8 →
 *   alpha_over over active tracks → rgba8_to_frame → feed encoder.
 */
#pragma once

#include "orchestrator/output_sink.hpp"

#include <memory>

namespace me { struct Timeline; }

namespace me::orchestrator {

/* Constructs a ComposeSink for a multi-track video timeline. Returns
 * nullptr (and writes a diagnostic to *err) on argument validation
 * failures — currently: non-h264 / non-aac codec combinations (compose
 * only supports the reencode path; stream-copy cannot composite).
 *
 * `tl` is borrowed and must outlive the returned sink (the Exporter
 * keeps the Timeline alive for the lifetime of the render job). `pool`
 * is the engine's CodecPool — required because compose always runs
 * through re-encode (no passthrough compose is physically possible). */
std::unique_ptr<OutputSink> make_compose_sink(
    const me::Timeline&            tl,
    const me_output_spec_t&        spec,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    std::string*                   err);

}  // namespace me::orchestrator
