/*
 * test_compose_face_mosaic_stage — pins the compose-graph
 * topology change for M11 face-mosaic-compose-graph-stage-impl.
 * Sibling of test_compose_face_sticker_stage; same hand-built-
 * Timeline pattern, asserts that a clip with a `face_mosaic`
 * effect produces a `RenderFaceMosaic` node and that its
 * properties carry the kernel's required inputs (landmark URI,
 * frame_t, block_size_px, mosaic_kind).
 *
 * Pixel correctness lives in tests/test_face_mosaic_stub.cpp; this
 * test only verifies the orchestrator wiring — register-on-engine-
 * create + dispatch in compose_compile.cpp's append_clip_effects.
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
    tl.assets["landmark_a"] = Asset{.uri = "/tmp/me_test_face_mosaic_stage.json"};
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

EffectSpec face_mosaic_fx(bool enabled = true,
                           me::FaceMosaicEffectParams::Kind kind =
                               me::FaceMosaicEffectParams::Kind::Pixelate,
                           int block_size = 12) {
    EffectSpec fx;
    fx.kind    = EffectKind::FaceMosaic;
    fx.enabled = enabled;
    FaceMosaicEffectParams p;
    p.landmark.asset_id = "landmark_a";
    p.block_size_px     = block_size;
    p.kind              = kind;
    fx.params = p;
    return fx;
}

std::size_t count_kind(const graph::Graph& g, task::TaskKindId k) {
    std::size_t n = 0;
    for (const auto& node : g.nodes()) {
        if (node.kind == k) ++n;
    }
    return n;
}

void ensure_kernels_registered() {
    static std::once_flag once;
    std::call_once(once, []() {
        me_engine_t* eng = nullptr;
        me_engine_create(nullptr, &eng);
        if (eng) me_engine_destroy(eng);
    });
}

}  // namespace

TEST_CASE("compose graph: clip with face_mosaic effect inserts RenderFaceMosaic") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({ face_mosaic_fx() });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 4);  /* Demux → Decode → Convert → FaceMosaic */
    CHECK(count_kind(g, task::TaskKindId::RenderFaceMosaic) == 1);
}

TEST_CASE("compose graph: disabled face_mosaic effect is skipped") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({ face_mosaic_fx(/*enabled=*/false) });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 3);
    CHECK(count_kind(g, task::TaskKindId::RenderFaceMosaic) == 0);
}

TEST_CASE("compose graph: face_mosaic properties are populated from EffectSpec") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({
        face_mosaic_fx(/*enabled=*/true,
                       me::FaceMosaicEffectParams::Kind::Blur,
                       /*block_size=*/24)
    });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{15, 30},
                                                  &g, &terminal) == ME_OK);

    const graph::Node* fm_node = nullptr;
    for (const auto& node : g.nodes()) {
        if (node.kind == task::TaskKindId::RenderFaceMosaic) {
            fm_node = &node;
            break;
        }
    }
    REQUIRE(fm_node != nullptr);

    /* landmark_asset_uri resolved from `landmark_a` → tl.assets[...].uri. */
    auto lit = fm_node->props.find("landmark_asset_uri");
    REQUIRE(lit != fm_node->props.end());
    const auto* lp = std::get_if<std::string>(&lit->second.v);
    REQUIRE(lp != nullptr);
    CHECK(*lp == "/tmp/me_test_face_mosaic_stage.json");

    /* frame_t carries the compile-time `time` arg through. */
    auto ntit = fm_node->props.find("frame_t_num");
    auto dtit = fm_node->props.find("frame_t_den");
    REQUIRE(ntit != fm_node->props.end());
    REQUIRE(dtit != fm_node->props.end());
    const auto* nt_p = std::get_if<int64_t>(&ntit->second.v);
    const auto* dt_p = std::get_if<int64_t>(&dtit->second.v);
    REQUIRE(nt_p != nullptr);
    REQUIRE(dt_p != nullptr);
    CHECK(*nt_p == 15);
    CHECK(*dt_p == 30);

    /* block_size_px from the effect params. */
    auto bit = fm_node->props.find("block_size_px");
    REQUIRE(bit != fm_node->props.end());
    const auto* bp = std::get_if<int64_t>(&bit->second.v);
    REQUIRE(bp != nullptr);
    CHECK(*bp == 24);

    /* mosaic_kind: Blur → 1. */
    auto kit = fm_node->props.find("mosaic_kind");
    REQUIRE(kit != fm_node->props.end());
    const auto* kp = std::get_if<int64_t>(&kit->second.v);
    REQUIRE(kp != nullptr);
    CHECK(*kp == 1);
}

TEST_CASE("compose graph: face_mosaic + face_sticker effects chain in document order") {
    ensure_kernels_registered();
    EffectSpec sticker_fx;
    sticker_fx.kind    = EffectKind::FaceSticker;
    sticker_fx.enabled = true;
    {
        FaceStickerEffectParams p;
        p.landmark.asset_id = "landmark_a";
        p.sticker_uri       = "file:///tmp/sticker.png";
        p.scale_x = 1.0; p.scale_y = 1.0;
        sticker_fx.params = p;
    }

    Timeline tl = make_clip_with_effects({ face_mosaic_fx(), sticker_fx });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    /* Demux → Decode → Convert → FaceMosaic → FaceSticker */
    CHECK(g.nodes().size() == 5);
    CHECK(count_kind(g, task::TaskKindId::RenderFaceMosaic)  == 1);
    CHECK(count_kind(g, task::TaskKindId::RenderFaceSticker) == 1);
}

TEST_CASE("compose graph: face_mosaic Pixelate kind serializes to mosaic_kind=0") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({
        face_mosaic_fx(/*enabled=*/true,
                       me::FaceMosaicEffectParams::Kind::Pixelate,
                       /*block_size=*/8)
    });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);

    const graph::Node* fm_node = nullptr;
    for (const auto& node : g.nodes()) {
        if (node.kind == task::TaskKindId::RenderFaceMosaic) {
            fm_node = &node;
            break;
        }
    }
    REQUIRE(fm_node != nullptr);
    auto kit = fm_node->props.find("mosaic_kind");
    REQUIRE(kit != fm_node->props.end());
    const auto* kp = std::get_if<int64_t>(&kit->second.v);
    REQUIRE(kp != nullptr);
    CHECK(*kp == 0);
}
