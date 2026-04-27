/*
 * compile_audio_chunk_graph — audio-side counterpart to
 * compile_compose_graph (compose_frame.hpp).
 *
 * Builds a per-chunk audio graph for a (Timeline, time, target
 * params) tuple: per audio track build
 *
 *   IoDemux(uri) → IoDecodeAudio(source_t)
 *                → AudioResample(target_rate / fmt / channels)
 *                → [optional AudioTimestretch when clip carries
 *                   tempo animation, future extension]
 *
 * then if more than one track contributes feed all per-track outputs
 * into AudioMix's variadic input. The resulting AVFrame at the named
 * "audio" terminal carries FLTP samples at the requested target
 * params.
 *
 * Phase-1 scope:
 *   - Single chunk per evaluation. Caller drives the chunk loop
 *     (Player audio pacer / Exporter drain) and bumps `time` per call.
 *   - No per-track tempo animation today (Timeline IR has no tempo
 *     animated number on Clip). When a clip carries tempo metadata
 *     in a future schema iteration, this compiler grows an
 *     AudioTimestretch insertion based on the track's instance_key.
 *   - Returns ME_E_NOT_FOUND when no audio track contributes at
 *     `time` — callers treat as "silent chunk" (gap inside the
 *     timeline) and either skip or emit zeros.
 *
 * Caller orchestration (commit-13 future migration target): Player +
 * Exporter audio paths today still use src/audio/mixer.cpp's
 * streaming AudioMixer. This compile entry exists so the
 * orchestrator can switch over track-by-track without losing test
 * coverage; the AudioMixer + AudioTrackFeed surfaces stay alive
 * during the transition.
 */
#pragma once

#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"

namespace me::orchestrator {

struct AudioChunkParams {
    int target_rate     = 48000;     /* Hz */
    int target_channels = 2;
    /* AVSampleFormat encoded as int — phase-1 always FLTP (= 8). */
    int target_fmt      = 8;
};

/* Build the per-chunk audio graph at `time` against `tl`. Returns
 *   ME_OK              — `*out_graph` + `*out_terminal` populated;
 *                         terminal is named "audio" (AvFrameHandle).
 *   ME_E_NOT_FOUND     — no audio track contributes at this time.
 *   ME_E_INVALID_ARG   — null outputs or empty timeline.
 */
me_status_t compile_audio_chunk_graph(
    const me::Timeline&     tl,
    me_rational_t           time,
    const AudioChunkParams& params,
    graph::Graph*           out_graph,
    graph::PortRef*         out_terminal);

}  // namespace me::orchestrator
