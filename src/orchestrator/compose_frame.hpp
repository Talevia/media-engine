/*
 * Per-frame compose helpers — execution model (a) primitives.
 *
 * docs/ARCHITECTURE_GRAPH.md describes three execution models:
 *
 *   (a) per-frame, stateless    — what's in this file
 *   (b) streaming, stateful     — Exporter
 *   (c) playback session        — Player (composes (a) + (b))
 *
 * Path (a) is just a few free functions: compile the per-frame video
 * graph for a (Timeline, time) pair, evaluate it, optionally PNG-encode
 * the result.
 *
 * Consumers:
 *   - me_render_frame   (src/api/render.cpp)            — RGBA + disk_cache
 *   - me_player_t       (src/orchestrator/player.cpp)   — RGBA, no cache, with cancel
 *   - compose_png_at    (this header)                   — composition-level PNG
 *
 * compile_compose_graph walks every video track in declaration order
 * (= bottom→top z-order), at each track resolving either a single
 * active clip or an active cross-dissolve transition, then composing
 * the per-track outputs via RenderComposeCpu when more than one
 * contributes. Single-clip-no-transform-no-transition collapses to
 * the simple 3-node `IoDemux → IoDecodeVideo → RenderConvertRgba8`
 * graph (same shape M1 used) so that case stays cheap.
 */
#pragma once

#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

struct me_engine;

namespace me::orchestrator {

/* Build the per-frame video graph for `time` against `tl`.
 *
 * Topology depends on what's active at `time`:
 *
 *   • Exactly one video track contributes a single clip with no
 *     transform → 3-node chain (M1 shape):
 *       IoDemux → IoDecodeVideo → RenderConvertRgba8
 *
 *   • Otherwise (multi-layer, transform, or active transition):
 *     per layer build the 3-node chain, then either RenderAffineBlit
 *     (if the layer has a Transform or the canvas needs a resize) and
 *     RenderCrossDissolve (if the layer is in a transition window).
 *     All N per-layer outputs feed RenderComposeCpu's variadic input.
 *
 * Returns ME_E_NOT_FOUND when no video track contributes at `time`
 * (every track has either no active clip or a disabled track). The
 * Graph is move-only and the out-pair owns it. Caller must keep the
 * Graph alive until any scheduler.evaluate_port future against it
 * has been awaited — sched::EvalInstance stores the graph by const
 * reference (eval_instance.hpp:66). */
me_status_t compile_compose_graph(
    const me::Timeline&  tl,
    me_rational_t        time,
    graph::Graph*        out_graph,
    graph::PortRef*      out_terminal);

/* Resolve + compile + submit + await for the timeline-coordinate
 * `time`. Single-shot, no cancel — long-lived consumers (Player)
 * inline their own resolve / submit so they can publish the Future
 * for cooperative cancel. me_render_frame, compose_png_at, and any
 * future "give me one frame" caller use this convenience entry. */
me_status_t compose_frame_at(
    me_engine*                                  engine,
    const me::Timeline&                         tl,
    me_rational_t                               time,
    std::shared_ptr<me::graph::RgbaFrameData>*  out_rgba,
    std::string*                                err);

/* Composition-level PNG: compose_frame_at + scale-to-fit + libavcodec
 * PNG encode. `max_width` / `max_height` are caps; either zero means
 * "no cap on that axis" (output preserves source aspect ratio).
 * Caller frees `*out_png` via me_buffer_free.
 *
 * NOT the asset-level thumbnail path (`me_thumbnail_png` in
 * src/api/thumbnail.cpp). That one takes a plain URI without a
 * Timeline. The two roles are separate. */
me_status_t compose_png_at(
    me_engine*           engine,
    const me::Timeline&  tl,
    me_rational_t        time,
    int                  max_width,
    int                  max_height,
    uint8_t**            out_png,
    size_t*              out_size,
    std::string*         err);

}  // namespace me::orchestrator
