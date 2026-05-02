/*
 * test_compose_face_sticker_stage — pins the compose-graph
 * topology change introduced for M11 face-sticker-compose-graph-
 * stage-impl: clips with a `face_sticker` effect get a
 * `RenderFaceSticker` node inserted between
 * `RenderConvertRgba8` and the post-clip pipeline (AffineBlit /
 * ComposeCpu).
 *
 * Builds Timeline IR by hand (avoids JSON loader's mp4-existence
 * checks) and asserts:
 *
 *   1. Single video track + clip with no effects → 3-node graph
 *      (M1 backward compat — adding the effect-dispatch path
 *      didn't break the no-effect case).
 *   2. Single video track + clip with one `face_sticker` effect
 *      → 4-node graph: Demux → Decode → Convert → FaceSticker.
 *   3. Single video track + clip with `enabled=false` face_sticker
 *      effect → 3-node graph (disabled effects skipped per
 *      EffectSpec semantics).
 *   4. Two effects in document order → 5-node graph (currently
 *      only face_sticker is wired; a sibling Color effect is
 *      silently passed through, so 4 nodes total — pinning the
 *      "unknown effect = no-op insert" semantic).
 *
 * Pixel correctness of `apply_face_sticker_inplace` lives in
 * tests/test_face_sticker_kernel_pixel.cpp (cycle 29473c8); this
 * test only verifies the orchestrator wiring.
 */
#include <doctest/doctest.h>

#include "graph/graph.hpp"
#include "media_engine/engine.h"
#include "orchestrator/compose_frame.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

#include <mutex>
#include <string>

using namespace me;

namespace {

Timeline make_clip_with_effects(std::vector<EffectSpec> effects) {
    Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.duration   = me_rational_t{1, 1};
    tl.width  = 640;
    tl.height = 480;
    tl.assets["video_a"]    = Asset{.uri = "file:///dev/null"};
    tl.assets["landmark_a"] = Asset{.uri = "/tmp/me_test_face_sticker_stage.json"};
    tl.tracks.push_back(Track{.id = "video_0", .kind = TrackKind::Video, .enabled = true});
    Clip c;
    c.id            = "clip_a";
    c.asset_id      = "video_a";
    c.track_id      = "video_0";
    c.type          = ClipType::Video;
    c.time_start    = me_rational_t{0, 1};
    c.time_duration = me_rational_t{1, 1};
    c.source_start  = me_rational_t{0, 1};
    c.effects       = std::move(effects);
    tl.clips.push_back(std::move(c));
    return tl;
}

EffectSpec face_sticker_fx(bool enabled = true) {
    EffectSpec fx;
    fx.kind    = EffectKind::FaceSticker;
    fx.enabled = enabled;
    FaceStickerEffectParams p;
    p.landmark.asset_id = "landmark_a";
    p.sticker_uri       = "file:///tmp/sticker.png";
    p.scale_x = 1.0; p.scale_y = 1.0;
    fx.params = p;
    return fx;
}

EffectSpec color_fx(bool enabled = true) {
    EffectSpec fx;
    fx.kind    = EffectKind::Color;
    fx.enabled = enabled;
    fx.params  = ColorEffectParams{};
    return fx;
}

/* Count nodes in a graph by TaskKindId. */
std::size_t count_kind(const graph::Graph& g, task::TaskKindId k) {
    std::size_t n = 0;
    for (const auto& node : g.nodes()) {
        if (node.kind == k) ++n;
    }
    return n;
}

/* Engine kernels are registered by me_engine_create's init.
 * Builder::add throws on unregistered TaskKindId so each test
 * primes the registry through one engine create/destroy cycle.
 * Registration is process-wide + idempotent (same shape as
 * test_compose_multi_track_graph.cpp's bootstrap). */
void ensure_kernels_registered() {
    static std::once_flag once;
    std::call_once(once, []() {
        me_engine_t* eng = nullptr;
        me_engine_create(nullptr, &eng);
        if (eng) me_engine_destroy(eng);
    });
}

}  // namespace

TEST_CASE("compose graph: clip with no effects produces 3-node M1 chain") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({});
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 3);
    CHECK(count_kind(g, task::TaskKindId::IoDemux)            == 1);
    CHECK(count_kind(g, task::TaskKindId::IoDecodeVideo)      == 1);
    CHECK(count_kind(g, task::TaskKindId::RenderConvertRgba8) == 1);
    CHECK(count_kind(g, task::TaskKindId::RenderFaceSticker)  == 0);
}

TEST_CASE("compose graph: clip with face_sticker effect inserts RenderFaceSticker") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({ face_sticker_fx() });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 4);
    CHECK(count_kind(g, task::TaskKindId::RenderFaceSticker) == 1);
}

TEST_CASE("compose graph: disabled face_sticker effect is skipped") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({ face_sticker_fx(/*enabled=*/false) });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 3);  /* face_sticker not inserted */
    CHECK(count_kind(g, task::TaskKindId::RenderFaceSticker) == 0);
}

TEST_CASE("compose graph: unknown effect kinds (Color etc.) are silently passed through") {
    ensure_kernels_registered();
    /* Color stage isn't wired yet (cycle pending). The dispatch
     * in append_clip_effects falls through for kinds it doesn't
     * know — so a [Color, FaceSticker] effect list produces only
     * the FaceSticker node, not Color. This pins the "unknown =
     * no-op" semantic; when Color stage lands, this test should
     * be updated to assert 5 nodes (3 chain + Color + FaceSticker). */
    Timeline tl = make_clip_with_effects({ color_fx(), face_sticker_fx() });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 4);
    CHECK(count_kind(g, task::TaskKindId::RenderFaceSticker) == 1);
}

TEST_CASE("compose graph: face_sticker properties are populated from EffectSpec") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({ face_sticker_fx() });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{15, 30},
                                                  &g, &terminal) == ME_OK);

    /* Find the FaceSticker node. */
    const graph::Node* fs_node = nullptr;
    for (const auto& node : g.nodes()) {
        if (node.kind == task::TaskKindId::RenderFaceSticker) {
            fs_node = &node;
            break;
        }
    }
    REQUIRE(fs_node != nullptr);

    /* sticker_uri came verbatim from FaceStickerEffectParams. */
    auto sit = fs_node->props.find("sticker_uri");
    REQUIRE(sit != fs_node->props.end());
    const auto* sp = std::get_if<std::string>(&sit->second.v);
    REQUIRE(sp != nullptr);
    CHECK(*sp == "file:///tmp/sticker.png");

    /* landmark_asset_uri was resolved from `landmark_a` →
     * tl.assets["landmark_a"].uri. */
    auto lit = fs_node->props.find("landmark_asset_uri");
    REQUIRE(lit != fs_node->props.end());
    const auto* lp = std::get_if<std::string>(&lit->second.v);
    REQUIRE(lp != nullptr);
    CHECK(*lp == "/tmp/me_test_face_sticker_stage.json");

    /* frame_t carries the compile-time `time` arg through. */
    auto ntit = fs_node->props.find("frame_t_num");
    auto dtit = fs_node->props.find("frame_t_den");
    REQUIRE(ntit != fs_node->props.end());
    REQUIRE(dtit != fs_node->props.end());
    const auto* nt_p = std::get_if<int64_t>(&ntit->second.v);
    const auto* dt_p = std::get_if<int64_t>(&dtit->second.v);
    REQUIRE(nt_p != nullptr);
    REQUIRE(dt_p != nullptr);
    CHECK(*nt_p == 15);
    CHECK(*dt_p == 30);
}
