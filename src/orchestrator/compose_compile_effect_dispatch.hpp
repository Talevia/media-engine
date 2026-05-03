/* compose_compile_effect_dispatch — per-EffectKind →
 * compose-graph-Render*-node dispatch, extracted from
 * compose_compile.cpp.
 *
 * Why a separate TU. Pre-extraction `compose_compile.cpp` was
 * 561 lines, over the project's 400-line debt threshold (per
 * SKILL.md §R.5). The growth was structural: as M11 + M12
 * effects landed (16 effect kinds at the time of the split),
 * the per-kind dispatch arms in `append_clip_effects` grew to
 * ~270 lines on their own — a clean architectural split point.
 *
 * Surface. The function lives in `me::orchestrator::detail`
 * (cross-TU but not part of the public API). Only
 * compose_compile.cpp consumes it; if a future caller needs
 * the same dispatch they get the function from this header.
 *
 * Behavior contract. Walks `clip.effects` in document order.
 * For each enabled effect with a registered EffectKind, adds
 * a Render* graph node and updates `prev` to the new node's
 * output. Unknown / unregistered EffectKind values pass
 * through (no-op insert) so timeline JSON with mixed effects
 * doesn't break the graph. Returns the terminal RGBA8 port
 * after the last effect node, or `prev` unchanged when no
 * effects apply. */
#pragma once

#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"

namespace me::orchestrator::detail {

graph::PortRef append_clip_effects(graph::Graph::Builder& b,
                                    const me::Timeline&    tl,
                                    const me::Clip&        clip,
                                    graph::PortRef         prev,
                                    me_rational_t          time);

}  // namespace me::orchestrator::detail
