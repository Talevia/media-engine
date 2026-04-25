/*
 * test_decode_kernels — first end-to-end exercise of the new
 * IoDemux + IoDecodeVideo + RenderConvertRgba8 pipeline through
 * the scheduler. Builds a three-node graph against the
 * deterministic fixture and asserts:
 *
 *   1. evaluate_port<RgbaFrame> produces a non-empty RGBA buffer
 *      whose dimensions match the fixture (640×480).
 *   2. Two evaluations at the same asset-local time produce
 *      byte-identical output (sub-clock determinism check).
 *
 * Reaches into src/ for graph/task/scheduler internals — the C
 * API doesn't expose graph evaluation, by design (see
 * docs/ARCHITECTURE_GRAPH.md §"明确不做": "暴露 Task / EvalInstance
 * 到公共 API").
 */
#include <doctest/doctest.h>

#include "compose/convert_rgba8_kernel.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "io/decode_video_kernel.hpp"
#include "io/demux_kernel.hpp"
#include "resource/codec_pool.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"
#include "task/task_kind.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

using namespace me;

namespace {

void register_kernels_once() {
    static std::once_flag once;
    std::call_once(once, []() {
        io::register_demux_kind();
        io::register_decode_video_kind();
        compose::register_convert_rgba8_kind();
    });
}

/* Build the standard three-node decode graph: demux → decode → rgba8.
 * source_t is asset-local time as a rational. */
std::pair<graph::Graph, graph::PortRef>
build_decode_graph(const std::string& uri,
                    int64_t            source_t_num,
                    int64_t            source_t_den) {
    graph::Graph::Builder b;

    /* IoDemux(uri) → DemuxContext */
    graph::Properties demux_props;
    demux_props["uri"].v = uri;
    auto n_demux = b.add(task::TaskKindId::IoDemux,
                          std::move(demux_props), {});

    /* IoDecodeVideo(source_t) takes DemuxContext, emits AVFrame */
    graph::Properties dec_props;
    dec_props["source_t_num"].v = source_t_num;
    dec_props["source_t_den"].v = source_t_den;
    auto n_decode = b.add(task::TaskKindId::IoDecodeVideo,
                           std::move(dec_props),
                           { graph::PortRef{n_demux, 0} });

    /* RenderConvertRgba8 takes AVFrame, emits RgbaFrameData */
    auto n_rgba = b.add(task::TaskKindId::RenderConvertRgba8,
                         {},
                         { graph::PortRef{n_decode, 0} });

    graph::PortRef terminal{n_rgba, 0};
    b.name_terminal("rgba", terminal);
    return {std::move(b).build(), terminal};
}

}  // namespace

TEST_CASE("decode kernels: three-node graph produces RGBA8 frame") {
    register_kernels_once();

    const std::string uri = std::string("file://") + ME_TEST_FIXTURE_MP4;
    auto [g, term] = build_decode_graph(uri, /*num=*/1, /*den=*/30);

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1}, frames, codecs);

    graph::EvalContext ctx;
    ctx.frames = &frames;
    ctx.codecs = &codecs;

    auto fut = s.evaluate_port<std::shared_ptr<graph::RgbaFrameData>>(g, term, ctx);
    auto rgba = fut.await();

    REQUIRE(rgba != nullptr);
    /* gen_fixture produces a 640×480 MPEG-4 Part 2 video. */
    CHECK(rgba->width  == 640);
    CHECK(rgba->height == 480);
    CHECK(rgba->stride == 640u * 4u);
    CHECK(rgba->rgba.size() == 640u * 480u * 4u);
}

TEST_CASE("decode kernels: same source_t → byte-identical RGBA") {
    register_kernels_once();

    const std::string uri = std::string("file://") + ME_TEST_FIXTURE_MP4;

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1}, frames, codecs);

    auto evaluate = [&](int64_t num, int64_t den) {
        auto [g, term] = build_decode_graph(uri, num, den);
        graph::EvalContext ctx;
        ctx.frames = &frames;
        ctx.codecs = &codecs;
        auto fut = s.evaluate_port<std::shared_ptr<graph::RgbaFrameData>>(g, term, ctx);
        return fut.await();
    };

    auto a = evaluate(2, 30);
    auto b = evaluate(2, 30);

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->width  == b->width);
    REQUIRE(a->height == b->height);
    REQUIRE(a->rgba.size() == b->rgba.size());
    /* MPEG-4 Part 2 software decode + sws_scale BILINEAR is
     * byte-deterministic on the same machine. */
    CHECK(a->rgba == b->rgba);
}

TEST_CASE("decode kernels: graph content_hash stable across rebuilds") {
    register_kernels_once();

    const std::string uri = std::string("file://") + ME_TEST_FIXTURE_MP4;
    auto [g1, _t1] = build_decode_graph(uri, 1, 30);
    auto [g2, _t2] = build_decode_graph(uri, 1, 30);
    CHECK(g1.content_hash() == g2.content_hash());

    /* Different source_t → different hash (proves source_t goes
     * through props into the content hash, supporting D1). */
    auto [g3, _t3] = build_decode_graph(uri, 2, 30);
    CHECK(g1.content_hash() != g3.content_hash());
}
