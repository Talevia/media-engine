/*
 * test_io_decode_audio — pins the IoDecodeAudio kernel against the
 * audio-capable determinism fixture.
 *
 * Covers:
 *   1. Demux + DecodeAudio chain produces a non-empty AVFrame at
 *      source_t = 0.
 *   2. Repeat decode at the same source_t yields a frame with the
 *      same nb_samples (deterministic).
 *   3. Schema introspection (DemuxCtx in / AvFrameHandle out, source_t
 *      params declared).
 */
#include <doctest/doctest.h>

#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "io/decode_audio_kernel.hpp"
#include "io/demux_kernel.hpp"
#include "resource/codec_pool.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

extern "C" {
#include <libavutil/frame.h>
}

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
        io::register_decode_audio_kind();
    });
}

std::pair<graph::Graph, graph::PortRef>
build_decode_audio_graph(const std::string& uri,
                          int64_t            source_num,
                          int64_t            source_den) {
    graph::Graph::Builder b;

    graph::Properties demux_props;
    demux_props["uri"].v = uri;
    auto n_demux = b.add(task::TaskKindId::IoDemux,
                          std::move(demux_props), {});

    graph::Properties dec_props;
    dec_props["source_t_num"].v = source_num;
    dec_props["source_t_den"].v = source_den;
    auto n_dec = b.add(task::TaskKindId::IoDecodeAudio,
                        std::move(dec_props),
                        { graph::PortRef{n_demux, 0} });

    graph::PortRef terminal{n_dec, 0};
    b.name_terminal("audio", terminal);
    return {std::move(b).build(), terminal};
}

}  // namespace

TEST_CASE("IoDecodeAudio: 3-node graph produces audio AVFrame") {
    register_kernels_once();

    const std::string uri = std::string("file://") + ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    auto [g, term] = build_decode_audio_graph(uri, /*num=*/0, /*den=*/1);

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1}, frames, codecs);

    graph::EvalContext ctx;
    ctx.frames = &frames;
    ctx.codecs = &codecs;

    auto fut = s.evaluate_port<std::shared_ptr<AVFrame>>(g, term, ctx);
    auto frame = fut.await();

    REQUIRE(frame != nullptr);
    CHECK(frame->nb_samples > 0);
    CHECK(frame->ch_layout.nb_channels > 0);
    CHECK(frame->sample_rate > 0);
}

TEST_CASE("IoDecodeAudio: same source_t → same nb_samples") {
    register_kernels_once();

    const std::string uri = std::string("file://") + ME_TEST_FIXTURE_MP4_WITH_AUDIO;

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1}, frames, codecs);

    auto evaluate = [&](int64_t num, int64_t den) {
        auto [g, term] = build_decode_audio_graph(uri, num, den);
        graph::EvalContext ctx;
        ctx.frames = &frames;
        ctx.codecs = &codecs;
        auto fut = s.evaluate_port<std::shared_ptr<AVFrame>>(g, term, ctx);
        return fut.await();
    };

    auto a = evaluate(0, 1);
    auto b = evaluate(0, 1);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->nb_samples == b->nb_samples);
    CHECK(a->ch_layout.nb_channels == b->ch_layout.nb_channels);
}

TEST_CASE("IoDecodeAudio: schema declared correctly") {
    register_kernels_once();
    const auto* info = task::lookup(task::TaskKindId::IoDecodeAudio);
    REQUIRE(info);
    REQUIRE(info->input_schema.size() == 1);
    CHECK(info->input_schema[0].type == graph::TypeId::DemuxCtx);
    REQUIRE(info->output_schema.size() == 1);
    CHECK(info->output_schema[0].type == graph::TypeId::AvFrameHandle);
    CHECK(info->time_invariant == false);
    CHECK(info->cacheable == true);
    REQUIRE(info->param_schema.size() == 2);
    CHECK(info->param_schema[0].name == "source_t_num");
    CHECK(info->param_schema[1].name == "source_t_den");
}
