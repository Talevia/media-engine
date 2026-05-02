/*
 * test_compose_body_alpha_key_stage — pins the compose-graph
 * topology change for M11 body-alpha-key-compose-graph-stage-impl.
 * Sibling of test_compose_face_sticker_stage; same hand-built-
 * Timeline pattern, asserts that a clip with a `body_alpha_key`
 * effect produces a `RenderBodyAlphaKey` node and that its
 * properties carry the kernel's required inputs (mask URI,
 * frame_t, feather_radius_px, invert).
 *
 * Pixel correctness lives in tests/test_body_alpha_key_*.cpp;
 * this test only verifies the orchestrator wiring — register-on-
 * engine-create + dispatch in compose_compile.cpp's
 * append_clip_effects.
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
    tl.assets["video_a"] = Asset{.uri = "file:///dev/null"};
    tl.assets["mask_a"]  = Asset{.uri = "/tmp/me_test_body_alpha_key_stage.json"};
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

EffectSpec body_alpha_key_fx(bool enabled = true,
                              int  feather  = 0,
                              bool invert   = false) {
    EffectSpec fx;
    fx.kind    = EffectKind::BodyAlphaKey;
    fx.enabled = enabled;
    BodyAlphaKeyEffectParams p;
    p.mask.asset_id     = "mask_a";
    p.feather_radius_px = feather;
    p.invert            = invert;
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

TEST_CASE("compose graph: clip with body_alpha_key effect inserts RenderBodyAlphaKey") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({ body_alpha_key_fx() });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 4);  /* Demux → Decode → Convert → BodyAlphaKey */
    CHECK(count_kind(g, task::TaskKindId::RenderBodyAlphaKey) == 1);
}

TEST_CASE("compose graph: disabled body_alpha_key effect is skipped") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({ body_alpha_key_fx(/*enabled=*/false) });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    CHECK(g.nodes().size() == 3);
    CHECK(count_kind(g, task::TaskKindId::RenderBodyAlphaKey) == 0);
}

TEST_CASE("compose graph: body_alpha_key properties are populated from EffectSpec") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({
        body_alpha_key_fx(/*enabled=*/true,
                           /*feather=*/8,
                           /*invert=*/true)
    });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{15, 30},
                                                  &g, &terminal) == ME_OK);

    const graph::Node* bak_node = nullptr;
    for (const auto& node : g.nodes()) {
        if (node.kind == task::TaskKindId::RenderBodyAlphaKey) {
            bak_node = &node;
            break;
        }
    }
    REQUIRE(bak_node != nullptr);

    /* mask_asset_uri resolved from `mask_a` → tl.assets[...].uri. */
    auto mit = bak_node->props.find("mask_asset_uri");
    REQUIRE(mit != bak_node->props.end());
    const auto* mp = std::get_if<std::string>(&mit->second.v);
    REQUIRE(mp != nullptr);
    CHECK(*mp == "/tmp/me_test_body_alpha_key_stage.json");

    /* frame_t carries the compile-time `time` arg through. */
    auto ntit = bak_node->props.find("frame_t_num");
    auto dtit = bak_node->props.find("frame_t_den");
    REQUIRE(ntit != bak_node->props.end());
    REQUIRE(dtit != bak_node->props.end());
    const auto* nt_p = std::get_if<int64_t>(&ntit->second.v);
    const auto* dt_p = std::get_if<int64_t>(&dtit->second.v);
    REQUIRE(nt_p != nullptr);
    REQUIRE(dt_p != nullptr);
    CHECK(*nt_p == 15);
    CHECK(*dt_p == 30);

    /* feather_radius_px from the effect params. */
    auto fit = bak_node->props.find("feather_radius_px");
    REQUIRE(fit != bak_node->props.end());
    const auto* fp = std::get_if<int64_t>(&fit->second.v);
    REQUIRE(fp != nullptr);
    CHECK(*fp == 8);

    /* invert: true → 1. */
    auto iit = bak_node->props.find("invert");
    REQUIRE(iit != bak_node->props.end());
    const auto* ip = std::get_if<int64_t>(&iit->second.v);
    REQUIRE(ip != nullptr);
    CHECK(*ip == 1);
}

TEST_CASE("compose graph: body_alpha_key invert=false serializes to invert=0") {
    ensure_kernels_registered();
    Timeline tl = make_clip_with_effects({
        body_alpha_key_fx(/*enabled=*/true, /*feather=*/0, /*invert=*/false)
    });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);

    const graph::Node* bak_node = nullptr;
    for (const auto& node : g.nodes()) {
        if (node.kind == task::TaskKindId::RenderBodyAlphaKey) {
            bak_node = &node;
            break;
        }
    }
    REQUIRE(bak_node != nullptr);
    auto iit = bak_node->props.find("invert");
    REQUIRE(iit != bak_node->props.end());
    const auto* ip = std::get_if<int64_t>(&iit->second.v);
    REQUIRE(ip != nullptr);
    CHECK(*ip == 0);
}

TEST_CASE("compose graph: body_alpha_key + face_sticker chain in document order") {
    ensure_kernels_registered();
    EffectSpec sticker_fx;
    sticker_fx.kind    = EffectKind::FaceSticker;
    sticker_fx.enabled = true;
    {
        FaceStickerEffectParams p;
        p.landmark.asset_id = "mask_a";  /* re-using mask_a as a landmark id is harmless here — wiring test only counts nodes */
        p.sticker_uri       = "file:///tmp/sticker.png";
        p.scale_x = 1.0; p.scale_y = 1.0;
        sticker_fx.params = p;
    }

    Timeline tl = make_clip_with_effects({ body_alpha_key_fx(), sticker_fx });
    graph::Graph g;
    graph::PortRef terminal{};
    REQUIRE(orchestrator::compile_compose_graph(tl, me_rational_t{0, 30},
                                                  &g, &terminal) == ME_OK);
    /* Demux → Decode → Convert → BodyAlphaKey → FaceSticker */
    CHECK(g.nodes().size() == 5);
    CHECK(count_kind(g, task::TaskKindId::RenderBodyAlphaKey) == 1);
    CHECK(count_kind(g, task::TaskKindId::RenderFaceSticker)  == 1);
}
