/* test_task_context_engine_threading — verifies that a non-NULL
 * `me_engine*` set on `me::graph::EvalContext::engine` reaches
 * the kernel via `me::task::TaskContext::engine`. Catches
 * regressions in scheduler.cpp's `ctx.engine = eval.ctx().engine`
 * forwarding (cycle 4f79c1c) and the EvalContext-construction
 * call sites (compose_frame.cpp, api/render.cpp,
 * api/thumbnail.cpp, orchestrator/player.cpp,
 * orchestrator/exporter.cpp, orchestrator/player_audio_producer.cpp).
 *
 * Test pattern: probe kernel captures `ctx.engine` into a file-
 * scope atomic pointer. We build a tiny one-node graph with the
 * probe kernel, dispatch via the scheduler with a sentinel
 * `me_engine*` (cast-from-int — the scheduler doesn't dereference
 * the engine pointer, just forwards it), and assert the captured
 * pointer matches.
 */
#include <doctest/doctest.h>

#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "media_engine/types.h"
#include "resource/codec_pool.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <atomic>
#include <cstdint>
#include <span>

using namespace me;

namespace {

/* File-scope capture of the engine pointer the kernel saw. */
std::atomic<me_engine*> g_captured_engine{nullptr};

me_status_t engine_probe_kernel(task::TaskContext&        ctx,
                                  const graph::Properties& props,
                                  std::span<const graph::InputValue>,
                                  std::span<graph::OutputSlot> outs) {
    g_captured_engine.store(ctx.engine, std::memory_order_release);
    auto it = props.find("v");
    if (it == props.end()) return ME_E_INVALID_ARG;
    outs[0].v = std::get<int64_t>(it->second.v);
    return ME_OK;
}

void register_engine_probe_kind() {
    task::reset_registry_for_testing();
    g_captured_engine.store(nullptr);

    task::KindInfo k{
        .kind           = task::TaskKindId::TestConstInt,
        .affinity       = task::Affinity::Cpu,
        .latency        = task::Latency::Short,
        .time_invariant = true,
        .kernel         = engine_probe_kernel,
        .input_schema   = {},
        .output_schema  = { {"value", graph::TypeId::Int64} },
        .param_schema   = { {.name = "v", .type = graph::TypeId::Int64} },
    };
    task::register_kind(k);
}

graph::Graph build_probe_graph() {
    graph::Graph::Builder b;
    graph::Properties p;
    p["v"].v = static_cast<int64_t>(7);
    auto n = b.add(task::TaskKindId::TestConstInt, std::move(p), {});
    b.name_terminal("value", graph::PortRef{n, 0});
    return std::move(b).build();
}

}  // namespace

TEST_CASE("TaskContext.engine: forwarded from EvalContext.engine") {
    register_engine_probe_kind();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1, .output_cache_capacity = 0},
                        frames, codecs);

    auto g     = build_probe_graph();
    auto term  = g.terminal("value").value();

    /* Sentinel pointer — scheduler doesn't dereference it, just
     * forwards. Casting from a stack-local lets the test compare
     * against a known address without standing up a real engine. */
    int sentinel_byte = 0;
    me_engine* fake_engine = reinterpret_cast<me_engine*>(&sentinel_byte);

    graph::EvalContext ctx;
    ctx.frames = &frames;
    ctx.codecs = &codecs;
    ctx.engine = fake_engine;

    auto fut = s.evaluate_port<int64_t>(g, term, ctx);
    int64_t v = fut.await();
    CHECK(v == 7);

    /* Engine pointer the kernel saw must equal the one we set on
     * EvalContext. */
    CHECK(g_captured_engine.load(std::memory_order_acquire) == fake_engine);
}

TEST_CASE("TaskContext.engine: NULL forwards as NULL") {
    register_engine_probe_kind();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1, .output_cache_capacity = 0},
                        frames, codecs);

    auto g     = build_probe_graph();
    auto term  = g.terminal("value").value();

    graph::EvalContext ctx;
    ctx.frames = &frames;
    ctx.codecs = &codecs;
    /* ctx.engine left at default nullptr. */

    auto fut = s.evaluate_port<int64_t>(g, term, ctx);
    int64_t v = fut.await();
    CHECK(v == 7);
    CHECK(g_captured_engine.load(std::memory_order_acquire) == nullptr);
}
