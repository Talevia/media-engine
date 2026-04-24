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
namespace me::gpu { class GpuBackend; }

namespace me::orchestrator {

/* True iff `gpu` is a GPU backend usable for accelerated compose:
 * non-null, reports `available()`, and its renderer is a real bgfx
 * backend (name starts with "bgfx-" and is not "bgfx-Noop" — the
 * Noop fallback claims available=true but doesn't write real pixels,
 * so it must not be treated as GPU-usable by compose kernels).
 *
 * Exposed publicly so the same predicate drives ComposeSink's path
 * selection + a unit-test harness that mocks GpuBackend subclasses
 * (compose_sink.cpp is otherwise entirely in an anonymous
 * namespace). */
bool is_gpu_compose_usable(const me::gpu::GpuBackend* gpu) noexcept;

/* Constructs a ComposeSink for a multi-track video timeline. Returns
 * nullptr (and writes a diagnostic to *err) on argument validation
 * failures — currently: non-h264 / non-aac codec combinations (compose
 * only supports the reencode path; stream-copy cannot composite).
 *
 * `tl` is borrowed and must outlive the returned sink (the Exporter
 * keeps the Timeline alive for the lifetime of the render job). `pool`
 * is the engine's CodecPool — required because compose always runs
 * through re-encode (no passthrough compose is physically possible).
 *
 * `gpu_backend` is the engine's GpuBackend (borrowed; may be null).
 * Today ComposeSink records it but always runs the CPU compose path;
 * future cycles (effect-gpu-* + pass-merge) flip the branch to route
 * through GPU when `is_gpu_compose_usable(gpu_backend)` returns true.
 * The parameter is plumbed now so that future cycle is local to
 * compose_sink.cpp rather than a cross-file signature change. */
std::unique_ptr<OutputSink> make_compose_sink(
    const me::Timeline&            tl,
    const me_output_spec_t&        spec,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    const me::gpu::GpuBackend*     gpu_backend,
    std::string*                   err);

}  // namespace me::orchestrator
