/*
 * test_audio_chunk_graph — pins compile_audio_chunk_graph topology +
 * end-to-end evaluation through the kernel pipeline.
 *
 * Builds Timeline IR by hand with a single audio track + clip
 * pointing at the audio-capable determinism fixture, compiles the
 * audio graph, evaluates it through the engine's scheduler, and
 * checks the mixed AVFrame matches the requested target params.
 *
 * The integration covers all four Phase B kernels in concert:
 *   IoDemux → IoDecodeAudio → AudioResample → AudioMix
 * (AudioTimestretch is exercised separately by
 * test_audio_timestretch_kernel; the chunk graph doesn't insert it
 * today since the Timeline schema has no per-clip tempo animation
 * for audio.)
 */
#include <doctest/doctest.h>

#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/engine.h"
#include "orchestrator/audio_graph.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"
#include "timeline/timeline_impl.hpp"

#include "core/engine_impl.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <memory>
#include <mutex>
#include <string>

using namespace me;

namespace {

void register_kernels_via_engine_once() {
    static std::once_flag once;
    std::call_once(once, []() {
        me_engine_t* eng = nullptr;
        me_engine_create(nullptr, &eng);
        if (eng) me_engine_destroy(eng);
    });
}

Timeline make_audio_only_timeline(const std::string& uri) {
    Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.duration   = me_rational_t{1, 1};
    tl.assets["asset_a"] = Asset{.uri = uri};
    tl.tracks.push_back(Track{.id = "audio_0", .kind = TrackKind::Audio, .enabled = true});
    Clip c;
    c.id            = "clip_a";
    c.asset_id      = "asset_a";
    c.track_id      = "audio_0";
    c.type          = ClipType::Audio;
    c.time_start    = me_rational_t{0, 1};
    c.time_duration = me_rational_t{1, 1};
    c.source_start  = me_rational_t{0, 1};
    tl.clips.push_back(std::move(c));
    return tl;
}

}  // namespace

TEST_CASE("compile_audio_chunk_graph: single track produces 4-node chain") {
    register_kernels_via_engine_once();

    Timeline tl = make_audio_only_timeline(
        std::string("file://") + ME_TEST_FIXTURE_MP4_WITH_AUDIO);
    orchestrator::AudioChunkParams params;
    params.target_rate     = 48000;
    params.target_channels = 2;
    params.target_fmt      = AV_SAMPLE_FMT_FLTP;

    graph::Graph   g;
    graph::PortRef term{};
    REQUIRE(orchestrator::compile_audio_chunk_graph(
                tl, me_rational_t{0, 1}, params, &g, &term) == ME_OK);
    /* Demux + DecodeAudio + Resample + Mix(1 input) = 4 nodes. */
    CHECK(g.nodes().size() == 4);
}

TEST_CASE("compile_audio_chunk_graph: cursor outside clip → ME_E_NOT_FOUND") {
    register_kernels_via_engine_once();
    Timeline tl = make_audio_only_timeline("file:///dev/null");
    orchestrator::AudioChunkParams params;
    graph::Graph   g;
    graph::PortRef term{};
    CHECK(orchestrator::compile_audio_chunk_graph(
              tl, me_rational_t{5, 1}, params, &g, &term) == ME_E_NOT_FOUND);
}

TEST_CASE("compile_audio_chunk_graph: end-to-end evaluation produces FLTP @ target") {
    register_kernels_via_engine_once();

    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);
    REQUIRE(eng);

    Timeline tl = make_audio_only_timeline(
        std::string("file://") + ME_TEST_FIXTURE_MP4_WITH_AUDIO);
    orchestrator::AudioChunkParams params;
    params.target_rate     = 48000;
    params.target_channels = 2;
    params.target_fmt      = AV_SAMPLE_FMT_FLTP;

    graph::Graph   g;
    graph::PortRef term{};
    REQUIRE(orchestrator::compile_audio_chunk_graph(
                tl, me_rational_t{0, 1}, params, &g, &term) == ME_OK);

    graph::EvalContext ctx;
    ctx.frames = eng->frames.get();
    ctx.codecs = eng->codecs.get();

    auto fut = eng->scheduler->evaluate_port<std::shared_ptr<AVFrame>>(g, term, ctx);
    auto frame = fut.await();

    REQUIRE(frame);
    CHECK(frame->format      == AV_SAMPLE_FMT_FLTP);
    CHECK(frame->sample_rate == 48000);
    CHECK(frame->ch_layout.nb_channels == 2);
    CHECK(frame->nb_samples > 0);

    me_engine_destroy(eng);
}
