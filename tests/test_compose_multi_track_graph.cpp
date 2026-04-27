/*
 * test_compose_multi_track_graph — pins compile_compose_graph topology.
 *
 * Builds Timeline IR by hand (avoids JSON loader's mp4-existence
 * checks) and asserts:
 *   1. Single video track, single clip, no transform → 3-node graph
 *      (IoDemux → IoDecodeVideo → RenderConvertRgba8). Same shape as
 *      M1 — preserves backward compat for the simple case.
 *   2. Empty active set (cursor past timeline end) → ME_E_NOT_FOUND.
 *   3. Single track + clip with non-identity transform → 4-node graph
 *      (chain + RenderAffineBlit).
 *   4. Two tracks contributing → 7-node graph (3 per track + 1
 *      RenderComposeCpu) since multi-track forces AffineBlit per
 *      layer (no transform → identity, but blit-to-canvas still
 *      runs to ensure all layers share canvas dims).
 *      Wait — under our impl, multi-track without transform forces
 *      AffineBlit so each layer becomes 4 nodes; 2 tracks = 8 + 1
 *      ComposeCpu = 9 nodes total.
 *
 * Each test exercises the topology, not the rendered pixels — pixel
 * correctness lives in test_render_affine_blit / test_render_compose_cpu /
 * test_render_cross_dissolve / test_compose_alpha_over et al.
 */
#include <doctest/doctest.h>

#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/engine.h"
#include "orchestrator/compose_frame.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

#include <mutex>
#include <set>
#include <string>

using namespace me;

namespace {

/* Hand-build a 1-track Timeline with one clip covering [0, 1) seconds. */
Timeline make_single_track(const std::string& uri = "file:///dev/null",
                           bool with_transform = false) {
    Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.duration   = me_rational_t{1, 1};
    tl.width  = 640;
    tl.height = 480;
    tl.assets["asset_a"] = Asset{.uri = uri};
    tl.tracks.push_back(Track{.id = "video_0", .kind = TrackKind::Video, .enabled = true});
    Clip c;
    c.id            = "clip_a";
    c.asset_id      = "asset_a";
    c.track_id      = "video_0";
    c.type          = ClipType::Video;
    c.time_start    = me_rational_t{0, 1};
    c.time_duration = me_rational_t{1, 1};
    c.source_start  = me_rational_t{0, 1};
    if (with_transform) {
        Transform t;
        t.translate_x = AnimatedNumber::from_static(50.0);
        c.transform   = std::move(t);
    }
    tl.clips.push_back(std::move(c));
    return tl;
}

Timeline make_two_track(const std::string& uri = "file:///dev/null") {
    Timeline tl = make_single_track(uri);
    tl.tracks.push_back(Track{.id = "video_1", .kind = TrackKind::Video, .enabled = true});
    Clip c2;
    c2.id            = "clip_b";
    c2.asset_id      = "asset_a";
    c2.track_id      = "video_1";
    c2.type          = ClipType::Video;
    c2.time_start    = me_rational_t{0, 1};
    c2.time_duration = me_rational_t{1, 1};
    c2.source_start  = me_rational_t{0, 1};
    tl.clips.push_back(std::move(c2));
    return tl;
}

/* Count nodes of a specific kind in the graph. */
int count_kind(const graph::Graph& g, task::TaskKindId k) {
    int n = 0;
    for (const auto& node : g.nodes()) {
        if (node.kind == k) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("compile_compose_graph: single-track-no-transform → 3-node M1 chain") {
    /* Engine kernels are registered by me_engine_create. The kinds we
     * touch here (IoDemux / IoDecodeVideo / RenderConvertRgba8) need
     * to be registered for Builder::add to accept them. The kernel
     * registration is process-wide + idempotent, so anything that ran
     * an engine before (even another test) primes them. To make this
     * test independent, register here too. */
    static std::once_flag once;
    std::call_once(once, []() {
        /* Pull the engine bootstrap path indirectly: create + destroy
         * an engine. Cheaper than reaching into the io::register_*
         * private headers — registration is idempotent. */
        me_engine_t* eng = nullptr;
        me_engine_create(nullptr, &eng);
        if (eng) me_engine_destroy(eng);
    });

    Timeline tl = make_single_track();
    graph::Graph   g;
    graph::PortRef term{};
    REQUIRE(orchestrator::compile_compose_graph(
                tl, me_rational_t{0, 1}, &g, &term) == ME_OK);
    CHECK(g.nodes().size() == 3);
    CHECK(count_kind(g, task::TaskKindId::IoDemux)            == 1);
    CHECK(count_kind(g, task::TaskKindId::IoDecodeVideo)      == 1);
    CHECK(count_kind(g, task::TaskKindId::RenderConvertRgba8) == 1);
    CHECK(count_kind(g, task::TaskKindId::RenderAffineBlit)   == 0);
    CHECK(count_kind(g, task::TaskKindId::RenderComposeCpu)   == 0);
}

TEST_CASE("compile_compose_graph: cursor past timeline end → ME_E_NOT_FOUND") {
    Timeline tl = make_single_track();
    graph::Graph   g;
    graph::PortRef term{};
    /* time = 5s, timeline duration = 1s → no clip covers, NOT_FOUND. */
    CHECK(orchestrator::compile_compose_graph(
              tl, me_rational_t{5, 1}, &g, &term) == ME_E_NOT_FOUND);
}

TEST_CASE("compile_compose_graph: with non-identity transform → adds AffineBlit") {
    Timeline tl = make_single_track("file:///dev/null", /*with_transform=*/true);
    graph::Graph   g;
    graph::PortRef term{};
    REQUIRE(orchestrator::compile_compose_graph(
                tl, me_rational_t{0, 1}, &g, &term) == ME_OK);
    CHECK(g.nodes().size() == 4);
    CHECK(count_kind(g, task::TaskKindId::RenderAffineBlit) == 1);
    CHECK(count_kind(g, task::TaskKindId::RenderComposeCpu) == 0);
}

TEST_CASE("compile_compose_graph: 2 tracks → ComposeCpu with 2 layers") {
    Timeline tl = make_two_track();
    graph::Graph   g;
    graph::PortRef term{};
    REQUIRE(orchestrator::compile_compose_graph(
                tl, me_rational_t{0, 1}, &g, &term) == ME_OK);
    /* 2 tracks × (Demux + Decode + ConvertRgba + AffineBlit) + 1 ComposeCpu = 9. */
    CHECK(g.nodes().size() == 9);
    CHECK(count_kind(g, task::TaskKindId::IoDemux)            == 2);
    CHECK(count_kind(g, task::TaskKindId::IoDecodeVideo)      == 2);
    CHECK(count_kind(g, task::TaskKindId::RenderConvertRgba8) == 2);
    CHECK(count_kind(g, task::TaskKindId::RenderAffineBlit)   == 2);
    CHECK(count_kind(g, task::TaskKindId::RenderComposeCpu)   == 1);
}

TEST_CASE("compile_compose_graph: disabled track skipped") {
    Timeline tl = make_two_track();
    tl.tracks[1].enabled = false;
    graph::Graph   g;
    graph::PortRef term{};
    REQUIRE(orchestrator::compile_compose_graph(
                tl, me_rational_t{0, 1}, &g, &term) == ME_OK);
    /* Only track 0 contributes; falls back to 3-node M1 shape. */
    CHECK(g.nodes().size() == 3);
}
