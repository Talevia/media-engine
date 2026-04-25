/*
 * Internal companion to compose_sink.{hpp,cpp} — the ComposeSink
 * class lives in compose_sink_impl.cpp; the public factory in
 * compose_sink.cpp validates inputs then delegates here.
 *
 * Split rationale: pre-split the file was 399 lines and one
 * compose-sink feature commit away from the §1a 400-line ceiling.
 * The ComposeSink class body (~240 lines of per-frame loop wiring)
 * and the public validation factory (~80 lines of codec / pool /
 * track-clip-count checks) had no overlap in dependencies — the
 * class needs the full FFmpeg + reencode + audio pipeline; the
 * validation only touches Timeline IR + spec strings. Splitting
 * the class out drops compose_sink.cpp to ~130 lines and creates
 * a sibling that's still under 400.
 *
 * This header stays under `src/orchestrator/` (private). Public
 * callers continue to go through `make_compose_sink` in
 * `compose_sink.hpp`; this `detail::` factory is for the
 * validation factory only.
 */
#pragma once

#include "orchestrator/compose_sink.hpp"

namespace me::orchestrator::detail {

std::unique_ptr<OutputSink> make_compose_sink_impl(
    const me::Timeline&            tl,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    const me::gpu::GpuBackend*     gpu_backend,
    int64_t                        video_bitrate,
    int64_t                        audio_bitrate);

}  // namespace me::orchestrator::detail
