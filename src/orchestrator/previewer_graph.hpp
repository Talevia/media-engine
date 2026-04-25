/*
 * Previewer's per-frame graph builder.
 *
 * Compiles the three-node Graph that Previewer::frame_at evaluates
 * each call:
 *
 *   IoDemux(uri)
 *     → IoDecodeVideo(source_t_num, source_t_den)
 *     → RenderConvertRgba8
 *
 * Lives in its own TU so the Previewer .cpp stays focused on
 * timeline lookup + cache + result wrapping. The caller owns the
 * returned Graph; the PortRef points at the RGBA terminal.
 *
 * Single-track only today. Multi-track / transition support is the
 * follow-up cycle that adds RenderComposeCpu / RenderCrossDissolve
 * kernels and a multi-input graph topology — see
 * docs/BACKLOG.md "previewer-multi-track-compose-graph".
 */
#pragma once

#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/types.h"

#include <string>
#include <utility>

namespace me::orchestrator {

/* Build the per-frame decode graph for a single asset URI at a
 * specific asset-local time. Returns the compiled Graph and the
 * PortRef at the "rgba" terminal (RgbaFrame typed). */
std::pair<graph::Graph, graph::PortRef>
compile_frame_graph(const std::string& uri,
                     me_rational_t      source_t);

}  // namespace me::orchestrator
