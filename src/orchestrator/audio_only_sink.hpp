/*
 * me::orchestrator::make_audio_only_sink — factory for timelines
 * containing only audio tracks (no video).
 *
 * Scope: complements `make_compose_sink` for the edge case of
 * pure-audio composition. Compose path needs a video track to
 * initialize the video encoder + canvas buffers; audio-only
 * timelines would otherwise fail at the compose factory's
 * "bottom track has no clips" check (bottom track = video track
 * convention breaks when there aren't any).
 *
 * AudioOnlySink reuses `build_audio_mixer_for_timeline` to
 * construct the per-clip mix pipeline, configures an AAC encoder
 * via `setup_h264_aac_encoder_mux(..., audio_only=true)` so the
 * output MP4 has exactly one audio stream (no video), drains the
 * mixer + encodes, muxes, writes trailer.
 *
 * Routing: exporter.cpp dispatches to this sink when
 * `tl.tracks` has no Video-kind entries but at least one Audio.
 */
#pragma once

#include "media_engine/render.h"
#include "media_engine/types.h"
#include "orchestrator/output_sink.hpp"
#include "timeline/timeline_impl.hpp"

#include <memory>
#include <string>
#include <vector>

namespace me::resource { class CodecPool; }

namespace me::orchestrator {

std::unique_ptr<OutputSink> make_audio_only_sink(
    const me::Timeline&            tl,
    const me_output_spec_t&        spec,
    SinkCommon                     common,
    std::vector<ClipTimeRange>     clip_ranges,
    me::resource::CodecPool*       pool,
    std::string*                   err);

}  // namespace me::orchestrator
