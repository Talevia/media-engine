/*
 * compose_compile — compile_compose_graph + per-clip subgraph builders.
 *
 * Extracted from `compose_frame.cpp` as part of the
 * `debt-split-compose-frame-cpp` cycle. The pre-split TU was 423 lines
 * (over the 400-line P1-default threshold per SKILL §R.5); carving the
 * graph-build concern into its own TU drops the remaining
 * `compose_frame.cpp` (per-frame render + PNG export) under the
 * threshold without churning the consumer surface.
 *
 * Scope: `compile_compose_graph` + the three file-local helpers it
 * needs (`asset_uri_for`, `needs_affine`, `build_clip_chain`). All
 * declarations live in the existing `orchestrator/compose_frame.hpp`
 * header — no new header was introduced. Both compose_compile.cpp and
 * compose_frame.cpp include the same header so the contract surface
 * stays single-sourced.
 */
#include "orchestrator/compose_frame.hpp"

#include "compose/active_clips.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "orchestrator/compose_compile_effect_dispatch.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace me::orchestrator {

namespace {

/* Fetch the resolved asset URI for a clip. Returns empty string if
 * the asset isn't found (malformed Timeline; loader normally rejects). */
std::string asset_uri_for(const me::Timeline& tl, std::size_t clip_idx) {
    if (clip_idx >= tl.clips.size()) return {};
    const me::Clip& c  = tl.clips[clip_idx];
    auto             it = tl.assets.find(c.asset_id);
    if (it == tl.assets.end()) return {};
    return it->second.uri;
}

/* Decide whether a clip needs an explicit RenderAffineBlit pass. The
 * goal is to keep the simple "one full-frame clip" case at three
 * nodes (the M1 path) while still applying the user's transform when
 * one is set on the clip. Spatial-identity transforms (scale=1,
 * rotation=0, translate=0) collapse to no-op even when present, so
 * there's no need to bake an AffineBlit node into the graph. */
bool needs_affine(const me::Clip& clip) {
    if (!clip.transform.has_value()) return false;
    const auto eval = clip.transform->evaluate_at(me_rational_t{0, 1});
    return !eval.spatial_identity();
}

/* Build the bare 3-node decode chain for one clip
 * (Demux → Decode → ConvertRgba8) and return the RGBA8 PortRef. The
 * AffineBlit decision is left to the compile_compose_graph caller's
 * pass 2 — keeping pass 1 bare guarantees the simple-case
 * single-track-no-transform-no-transition graph stays at 3 nodes. */
graph::PortRef build_clip_chain(graph::Graph::Builder& b,
                                 const std::string&     uri,
                                 me_rational_t          source_t) {
    graph::Properties demux_props;
    demux_props["uri"].v = uri;
    auto n_demux = b.add(task::TaskKindId::IoDemux,
                          std::move(demux_props), {});

    graph::Properties dec_props;
    dec_props["source_t_num"].v = static_cast<int64_t>(source_t.num);
    dec_props["source_t_den"].v = static_cast<int64_t>(source_t.den);
    auto n_decode = b.add(task::TaskKindId::IoDecodeVideo,
                           std::move(dec_props),
                           { graph::PortRef{n_demux, 0} });

    auto n_rgba = b.add(task::TaskKindId::RenderConvertRgba8,
                         {},
                         { graph::PortRef{n_decode, 0} });
    return graph::PortRef{n_rgba, 0};
}

/* Append per-clip effect stages — body extracted to
 * compose_compile_effect_dispatch.cpp. Anonymous-namespace
 * wrapper kept so the three call sites in this TU don't need
 * to namespace-qualify. */
graph::PortRef append_clip_effects(graph::Graph::Builder& b,
                                    const me::Timeline&    tl,
                                    const me::Clip&        clip,
                                    graph::PortRef         prev,
                                    me_rational_t          time) {
    return detail::append_clip_effects(b, tl, clip, prev, time);
}


}  // namespace

me_status_t compile_compose_graph(const me::Timeline& tl,
                                   me_rational_t       time,
                                   graph::Graph*       out_graph,
                                   graph::PortRef*     out_terminal) {
    if (!out_graph || !out_terminal) return ME_E_INVALID_ARG;
    if (tl.tracks.empty() || tl.clips.empty()) return ME_E_INVALID_ARG;

    /* Normalize input time. */
    if (time.den <= 0) time.den = 1;
    if (time.num <  0) time.num = 0;

    /* Two-pass build:
     *   Pass 1: walk video tracks, build per-clip decode chains (and
     *           cross-dissolve subgraphs for transition layers).
     *           Collect each contributing layer's RGBA8 PortRef + the
     *           metadata needed to decide whether AffineBlit-to-canvas
     *           is required at finalize time.
     *   Pass 2: based on the contributing layer count + per-layer
     *           transform/transition state, append AffineBlit when
     *           necessary and either return the single layer port (no
     *           ComposeCpu) or wire all layers into RenderComposeCpu.
     *
     * The two passes let single-track-no-transform-no-transition
     * timelines collapse to the 3-node M1 graph (Demux → Decode →
     * Convert), preserving backward compat for the simple case. */
    struct LayerInfo {
        graph::PortRef  rgba_port;      /* Post-Convert (or post-CrossDissolve) RGBA output. */
        bool            already_canvas_sized = false;
                                         /* True when port came from a CrossDissolve (always
                                          * canvas-sized) or a forced AffineBlit. False when
                                          * it's the bare Convert output. */
        const me::Clip* clip = nullptr;  /* Single-clip layer's clip (for transform lookup). null for transition. */
        me_rational_t   tl_local{0, 1};  /* Timeline-local time for transform.evaluate_at. */
    };
    std::vector<LayerInfo> layers;
    layers.reserve(tl.tracks.size());

    graph::Graph::Builder b;

    const int canvas_w = tl.width  > 0 ? tl.width  : 0;
    const int canvas_h = tl.height > 0 ? tl.height : 0;

    /* ------- Pass 1 — build per-layer chains ------- */
    for (std::size_t ti = 0; ti < tl.tracks.size(); ++ti) {
        const me::Track& track = tl.tracks[ti];
        if (track.kind != me::TrackKind::Video || !track.enabled) continue;

        const me::compose::FrameSource fs =
            me::compose::frame_source_at(tl, ti, time);
        if (fs.kind == me::compose::FrameSourceKind::None) continue;

        if (fs.kind == me::compose::FrameSourceKind::SingleClip) {
            const me::Clip&   clip = tl.clips[fs.single.clip_idx];
            const std::string uri  = asset_uri_for(tl, fs.single.clip_idx);
            if (uri.empty()) continue;

            const me_rational_t tl_local{
                time.num * clip.time_start.den - clip.time_start.num * time.den,
                time.den * clip.time_start.den};
            graph::PortRef p = build_clip_chain(b, uri, fs.single.source_time);
            p = append_clip_effects(b, tl, clip, p, time);
            layers.push_back(LayerInfo{p, /*already_canvas_sized=*/false,
                                        &clip, tl_local});
        } else {
            /* Transition. Always force AffineBlit on both endpoints —
             * cross-dissolve requires identical dims. The cross-dissolve
             * output is canvas-sized by construction. */
            const me::Clip& from_clip = tl.clips[fs.transition_from_clip_idx];
            const me::Clip& to_clip   = tl.clips[fs.transition_to_clip_idx];
            const std::string from_uri = asset_uri_for(tl, fs.transition_from_clip_idx);
            const std::string to_uri   = asset_uri_for(tl, fs.transition_to_clip_idx);
            if (from_uri.empty() || to_uri.empty()) continue;

            const me_rational_t from_tl_local{
                time.num * from_clip.time_start.den - from_clip.time_start.num * time.den,
                time.den * from_clip.time_start.den};
            const me_rational_t to_tl_local{
                time.num * to_clip.time_start.den - to_clip.time_start.num * time.den,
                time.den * to_clip.time_start.den};

            /* Force AffineBlit on both endpoints — cross_dissolve
             * needs same dims; we ensure canvas-sized RGBA8 via a
             * mandatory AffineBlit pass per endpoint. Effects on
             * either endpoint apply BEFORE the transition mix. */
            graph::PortRef from_rgba = build_clip_chain(b, from_uri, fs.transition_from_source_time);
            from_rgba = append_clip_effects(b, tl, from_clip, from_rgba, time);
            graph::PortRef to_rgba   = build_clip_chain(b, to_uri,   fs.transition_to_source_time);
            to_rgba = append_clip_effects(b, tl, to_clip, to_rgba, time);

            auto wrap = [&](graph::PortRef src, const me::Clip& cl,
                            me_rational_t tl_local) -> graph::PortRef {
                graph::Properties bp;
                bp["dst_w"].v = static_cast<int64_t>(canvas_w);
                bp["dst_h"].v = static_cast<int64_t>(canvas_h);
                if (cl.transform.has_value()) {
                    const auto eval = cl.transform->evaluate_at(tl_local);
                    bp["translate_x"].v  = eval.translate_x;
                    bp["translate_y"].v  = eval.translate_y;
                    bp["scale_x"].v      = eval.scale_x;
                    bp["scale_y"].v      = eval.scale_y;
                    bp["rotation_deg"].v = eval.rotation_deg;
                    bp["anchor_x"].v     = eval.anchor_x;
                    bp["anchor_y"].v     = eval.anchor_y;
                }
                auto n = b.add(task::TaskKindId::RenderAffineBlit, std::move(bp), { src });
                return graph::PortRef{n, 0};
            };
            graph::PortRef from_port = wrap(from_rgba, from_clip, from_tl_local);
            graph::PortRef to_port   = wrap(to_rgba,   to_clip,   to_tl_local);

            graph::Properties dis_props;
            dis_props["progress"].v = static_cast<double>(fs.transition.t);
            auto n_dis = b.add(task::TaskKindId::RenderCrossDissolve,
                                std::move(dis_props),
                                { from_port, to_port });
            layers.push_back(LayerInfo{
                graph::PortRef{n_dis, 0},
                /*already_canvas_sized=*/true,
                /*clip=*/nullptr,
                /*tl_local=*/me_rational_t{0, 1}});
        }
    }

    if (layers.empty()) return ME_E_NOT_FOUND;

    /* ------- Pass 2 — finalize topology ------- */
    auto wrap_in_affine = [&](const LayerInfo& l) -> graph::PortRef {
        graph::Properties bp;
        bp["dst_w"].v = static_cast<int64_t>(canvas_w);
        bp["dst_h"].v = static_cast<int64_t>(canvas_h);
        if (l.clip && l.clip->transform.has_value()) {
            const auto eval = l.clip->transform->evaluate_at(l.tl_local);
            bp["translate_x"].v  = eval.translate_x;
            bp["translate_y"].v  = eval.translate_y;
            bp["scale_x"].v      = eval.scale_x;
            bp["scale_y"].v      = eval.scale_y;
            bp["rotation_deg"].v = eval.rotation_deg;
            bp["anchor_x"].v     = eval.anchor_x;
            bp["anchor_y"].v     = eval.anchor_y;
        }
        auto n = b.add(task::TaskKindId::RenderAffineBlit,
                        std::move(bp),
                        { l.rgba_port });
        return graph::PortRef{n, 0};
    };

    graph::PortRef terminal;
    if (layers.size() == 1) {
        const LayerInfo& l = layers[0];
        if (l.clip != nullptr && needs_affine(*l.clip)) {
            /* Single clip with non-identity transform: append AffineBlit. */
            terminal = wrap_in_affine(l);
        } else {
            /* Either a transition (already canvas-sized) or a
             * single-clip-no-transform (3-node M1 graph). */
            terminal = l.rgba_port;
        }
    } else {
        /* Multi-layer: every layer must share canvas dims for
         * RenderComposeCpu. Wrap every clip layer in AffineBlit
         * (transform if present, identity-to-canvas otherwise);
         * transition layers are already canvas-sized. */
        std::vector<graph::PortRef> ins;
        ins.reserve(layers.size());
        for (const auto& l : layers) {
            if (l.already_canvas_sized && l.clip == nullptr) {
                /* Transition output. */
                ins.push_back(l.rgba_port);
            } else {
                ins.push_back(wrap_in_affine(l));
            }
        }

        graph::Properties cprops;
        cprops["dst_w"].v = static_cast<int64_t>(canvas_w);
        cprops["dst_h"].v = static_cast<int64_t>(canvas_h);
        auto n_compose = b.add(task::TaskKindId::RenderComposeCpu,
                                std::move(cprops),
                                std::span<const graph::PortRef>{
                                    ins.data(), ins.size()});
        terminal = graph::PortRef{n_compose, 0};
    }

    b.name_terminal("rgba", terminal);
    *out_graph    = std::move(b).build();
    *out_terminal = terminal;
    return ME_OK;
}

}  // namespace me::orchestrator
