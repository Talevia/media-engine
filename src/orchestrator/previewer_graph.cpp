#include "orchestrator/previewer_graph.hpp"

#include "task/task_kind.hpp"

namespace me::orchestrator {

std::pair<graph::Graph, graph::PortRef>
compile_frame_graph(const std::string& uri,
                     me_rational_t      source_t) {
    graph::Graph::Builder b;

    /* Node 0: IoDemux(uri) → DemuxContext */
    graph::Properties demux_props;
    demux_props["uri"].v = uri;
    auto n_demux = b.add(task::TaskKindId::IoDemux,
                          std::move(demux_props), {});

    /* Node 1: IoDecodeVideo(source_t) → AVFrame.
     * source_t lives in props (not ctx.time) so the resulting graph's
     * content_hash distinguishes evaluations at different asset-local
     * moments — this is what lets the OutputCache key disambiguate
     * frames decoded from the same asset at different timestamps
     * (D1 in the implementation plan). */
    graph::Properties dec_props;
    dec_props["source_t_num"].v = static_cast<int64_t>(source_t.num);
    dec_props["source_t_den"].v = static_cast<int64_t>(source_t.den);
    auto n_decode = b.add(task::TaskKindId::IoDecodeVideo,
                           std::move(dec_props),
                           { graph::PortRef{n_demux, 0} });

    /* Node 2: RenderConvertRgba8 → RgbaFrameData (tightly-packed RGBA8) */
    auto n_rgba = b.add(task::TaskKindId::RenderConvertRgba8,
                         {},
                         { graph::PortRef{n_decode, 0} });

    graph::PortRef terminal{n_rgba, 0};
    b.name_terminal("rgba", terminal);
    return {std::move(b).build(), terminal};
}

}  // namespace me::orchestrator
