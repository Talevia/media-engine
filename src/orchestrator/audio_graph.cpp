#include "orchestrator/audio_graph.hpp"

#include "compose/active_clips.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace me::orchestrator {

namespace {

std::string asset_uri_for(const me::Timeline& tl, std::size_t clip_idx) {
    if (clip_idx >= tl.clips.size()) return {};
    const me::Clip& c  = tl.clips[clip_idx];
    auto             it = tl.assets.find(c.asset_id);
    if (it == tl.assets.end()) return {};
    return it->second.uri;
}

}  // namespace

me_status_t compile_audio_chunk_graph(const me::Timeline&     tl,
                                       me_rational_t           time,
                                       const AudioChunkParams& params,
                                       graph::Graph*           out_graph,
                                       graph::PortRef*         out_terminal) {
    if (!out_graph || !out_terminal) return ME_E_INVALID_ARG;
    if (tl.tracks.empty()) return ME_E_INVALID_ARG;
    if (params.target_rate <= 0 || params.target_channels <= 0) return ME_E_INVALID_ARG;

    if (time.den <= 0) time.den = 1;
    if (time.num <  0) time.num = 0;

    graph::Graph::Builder b;

    /* For each audio track, find the active clip and build a chain.
     * Audio doesn't have transitions in phase-1 schema — clips
     * butt-join, no cross-fade — so frame_source_at always reduces
     * to SingleClip / None for audio tracks. Use active_clips_at
     * directly (cheaper than frame_source_at). */
    const auto active = me::compose::active_clips_at(tl, time);

    struct Layer {
        graph::PortRef port;
        double         gain_db = 0.0;
    };
    std::vector<Layer> layers;
    layers.reserve(tl.tracks.size());

    for (const me::compose::TrackActive& ta : active) {
        if (ta.track_idx >= tl.tracks.size()) continue;
        const me::Track& track = tl.tracks[ta.track_idx];
        if (track.kind != me::TrackKind::Audio || !track.enabled) continue;

        const std::string uri = asset_uri_for(tl, ta.clip_idx);
        if (uri.empty()) continue;

        const me::Clip& clip = tl.clips[ta.clip_idx];

        /* IoDemux(uri) */
        graph::Properties demux_props;
        demux_props["uri"].v = uri;
        auto n_demux = b.add(task::TaskKindId::IoDemux,
                              std::move(demux_props), {});

        /* IoDecodeAudio(source_t) */
        graph::Properties dec_props;
        dec_props["source_t_num"].v = static_cast<int64_t>(ta.source_time.num);
        dec_props["source_t_den"].v = static_cast<int64_t>(ta.source_time.den);
        auto n_decode = b.add(task::TaskKindId::IoDecodeAudio,
                               std::move(dec_props),
                               { graph::PortRef{n_demux, 0} });

        /* AudioResample → target rate/fmt/channels. */
        graph::Properties rs_props;
        rs_props["target_rate"].v     = static_cast<int64_t>(params.target_rate);
        rs_props["target_fmt"].v      = static_cast<int64_t>(params.target_fmt);
        rs_props["target_channels"].v = static_cast<int64_t>(params.target_channels);
        auto n_resample = b.add(task::TaskKindId::AudioResample,
                                 std::move(rs_props),
                                 { graph::PortRef{n_decode, 0} });

        /* Optional AudioTimestretch — phase-1 schema has no per-clip
         * tempo animation on audio clips, so skip the kernel insertion
         * for now. Future: when clip.tempo (animated number) lands,
         * insert AudioTimestretch here with instance_key derived from
         * clip.id hash. */

        const double gain_db = clip.gain_db.has_value()
            ? clip.gain_db->evaluate_at(time)
            : 0.0;

        layers.push_back(Layer{graph::PortRef{n_resample, 0}, gain_db});
    }

    if (layers.empty()) return ME_E_NOT_FOUND;

    graph::PortRef terminal;
    if (layers.size() == 1) {
        /* Single audio track — apply gain via a 1-input AudioMix
         * (gain_db_0 prop) so the output uniformly carries the
         * track's level adjustment. */
        graph::Properties mix_props;
        mix_props["gain_db_0"].v = layers[0].gain_db;
        std::vector<graph::PortRef> ins{layers[0].port};
        auto n_mix = b.add(task::TaskKindId::AudioMix,
                            std::move(mix_props),
                            std::span<const graph::PortRef>{ins.data(), ins.size()});
        terminal = graph::PortRef{n_mix, 0};
    } else {
        graph::Properties mix_props;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            mix_props["gain_db_" + std::to_string(i)].v = layers[i].gain_db;
        }
        std::vector<graph::PortRef> ins;
        ins.reserve(layers.size());
        for (const auto& l : layers) ins.push_back(l.port);
        auto n_mix = b.add(task::TaskKindId::AudioMix,
                            std::move(mix_props),
                            std::span<const graph::PortRef>{ins.data(), ins.size()});
        terminal = graph::PortRef{n_mix, 0};
    }

    b.name_terminal("audio", terminal);
    *out_graph    = std::move(b).build();
    *out_terminal = terminal;
    return ME_OK;
}

}  // namespace me::orchestrator
