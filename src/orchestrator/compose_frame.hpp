/*
 * Per-frame compose helpers — execution model (a) primitives.
 *
 * docs/ARCHITECTURE_GRAPH.md describes three execution models:
 *
 *   (a) per-frame, stateless    — what's in this file
 *   (b) streaming, stateful     — Exporter
 *   (c) playback session        — Player (composes (a) + (b))
 *
 * Path (a) is just a few free functions: resolve which clip is active
 * at a given timeline-coordinate `time`, build the per-frame video
 * graph, evaluate it, optionally PNG-encode the result. Earlier
 * cycles wrapped each of these in a class (Previewer /
 * CompositionThumbnailer). The classes had no per-instance state and
 * no methods beyond a single one — pure bookkeeping. They were
 * deleted; the helpers below are the replacement.
 *
 * Consumers:
 *   - me_render_frame   (src/api/render.cpp)            — RGBA + disk_cache
 *   - me_player_t       (src/orchestrator/player.cpp)   — RGBA, no cache, with cancel
 *   - compose_png_at    (this header)                   — composition-level PNG
 *
 * Phase-1 limitation (matches the old Previewer): the active-clip
 * resolver only walks the bottom track. Multi-track compose is
 * gated on the `previewer-multi-track-compose-graph` backlog item;
 * when it lands, `compile_frame_graph` grows into something like
 * `compile_compose_graph(active_clips, t)` and `resolve_active_clip_at`
 * grows into `active_clips_at`. All callers below just pick up the
 * change at recompile time.
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

/* What `resolve_active_clip_at` returns: the asset URI to demux + the
 * clip-local time to seek/decode for. Both are the inputs to
 * `compile_frame_graph`. */
struct ResolvedClip {
    std::string    uri;
    me_rational_t  source_t{0, 1};
};

/* Find the active video clip at `time` in `tl`. Single-bottom-track
 * lookup (Phase-1 limitation; see file header). Returns:
 *   ME_OK                  — `*out` populated
 *   ME_E_NOT_FOUND         — `time` outside every clip's [start, end)
 *   ME_E_INVALID_ARG       — null tl / empty tracks / empty clips */
me_status_t resolve_active_clip_at(
    const me::Timeline&  tl,
    me_rational_t        time,
    ResolvedClip*        out);

/* Build the three-node per-frame video decode graph:
 *
 *   IoDemux(uri) → IoDecodeVideo(source_t) → RenderConvertRgba8
 *
 * The Graph object is move-only and the returned value-pair owns it.
 * The PortRef points at the "rgba" terminal (RgbaFrameData typed).
 *
 * Caller must keep the Graph alive until any
 * scheduler.evaluate_port future against it has been awaited —
 * sched::EvalInstance stores the graph by const reference
 * (eval_instance.hpp:66). */
std::pair<graph::Graph, graph::PortRef>
compile_frame_graph(const std::string& uri,
                     me_rational_t      source_t);

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
