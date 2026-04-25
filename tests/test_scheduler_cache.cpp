/*
 * test_scheduler_cache — proves the scheduler's OutputCache
 * peek-before-dispatch path:
 *
 *   1. time_invariant kernel: same graph + same EvalContext.time
 *      evaluated twice → kernel runs once, cache hits the second
 *      call (kernel-call counter stays at 1; cache.hit_count() goes up).
 *
 *   2. time-dependent kernel: same graph evaluated at distinct
 *      EvalContext.time values → kernel runs both times (mix_time
 *      makes the cache key distinct). Same time twice → hit again.
 *
 *   3. capacity = 0: cache is disabled, every evaluation runs the
 *      kernel (peek always misses, put no-ops).
 *
 *   4. cache fills to capacity then evicts oldest — bounded.
 *
 * Uses the bootstrap TestConstInt slot to reuse a known kind id
 * without crowding the production enum. Registry is reset at the
 * start of the binary; this binary doesn't depend on any kernel
 * registered by api/engine.cpp.
 */
#include <doctest/doctest.h>

#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "resource/codec_pool.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/output_cache.hpp"
#include "scheduler/scheduler.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <atomic>
#include <cstdint>
#include <span>

using namespace me;

namespace {

/* Two file-scope counters, one per kind. KernelFn is a raw fnptr
 * (no captures), so observable side-effects need static state. */
std::atomic<int> g_const_calls{0};
std::atomic<int> g_time_dep_calls{0};

/* time_invariant=true kernel — emits the int64 prop "v". */
me_status_t mock_const_kernel(task::TaskContext&,
                               const graph::Properties& props,
                               std::span<const graph::InputValue>,
                               std::span<graph::OutputSlot> outs) {
    g_const_calls.fetch_add(1, std::memory_order_relaxed);
    auto it = props.find("v");
    if (it == props.end()) return ME_E_INVALID_ARG;
    outs[0].v = std::get<int64_t>(it->second.v);
    return ME_OK;
}

/* time_invariant=false kernel — emits the int64 prop "v". The
 * scheduler still mixes ctx.time into the cache key, so two
 * evaluations at the same time hit; different times miss. */
me_status_t mock_time_dep_kernel(task::TaskContext&,
                                  const graph::Properties& props,
                                  std::span<const graph::InputValue>,
                                  std::span<graph::OutputSlot> outs) {
    g_time_dep_calls.fetch_add(1, std::memory_order_relaxed);
    auto it = props.find("v");
    if (it == props.end()) return ME_E_INVALID_ARG;
    outs[0].v = std::get<int64_t>(it->second.v);
    return ME_OK;
}

void register_mock_kinds_clean() {
    task::reset_registry_for_testing();
    g_const_calls.store(0);
    g_time_dep_calls.store(0);

    task::KindInfo k_const{
        .kind = task::TaskKindId::TestConstInt,
        .affinity = task::Affinity::Cpu,
        .latency = task::Latency::Short,
        .time_invariant = true,
        .kernel = mock_const_kernel,
        .input_schema  = {},
        .output_schema = { {"value", graph::TypeId::Int64} },
        .param_schema  = { {.name = "v", .type = graph::TypeId::Int64} },
    };
    task::register_kind(k_const);

    task::KindInfo k_time{
        .kind = task::TaskKindId::TestEchoString,   /* reused for time-dep mock */
        .affinity = task::Affinity::Cpu,
        .latency = task::Latency::Short,
        .time_invariant = false,                    /* <- key difference */
        .kernel = mock_time_dep_kernel,
        .input_schema  = {},
        .output_schema = { {"value", graph::TypeId::Int64} },
        .param_schema  = { {.name = "v", .type = graph::TypeId::Int64} },
    };
    task::register_kind(k_time);
}

graph::Graph build_const_graph(int64_t value) {
    graph::Graph::Builder b;
    graph::Properties p;
    p["v"].v = value;
    auto n = b.add(task::TaskKindId::TestConstInt, std::move(p), {});
    b.name_terminal("value", graph::PortRef{n, 0});
    return std::move(b).build();
}

graph::Graph build_time_dep_graph(int64_t value) {
    graph::Graph::Builder b;
    graph::Properties p;
    p["v"].v = value;
    auto n = b.add(task::TaskKindId::TestEchoString, std::move(p), {});
    b.name_terminal("value", graph::PortRef{n, 0});
    return std::move(b).build();
}

}  // namespace

TEST_CASE("OutputCache: time_invariant kernel runs once across two evaluations") {
    register_mock_kinds_clean();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1, .output_cache_capacity = 8},
                        frames, codecs);

    auto g = build_const_graph(42);
    graph::PortRef term{graph::NodeId{0}, 0};

    graph::EvalContext ctx;
    auto v1 = s.evaluate_port<int64_t>(g, term, ctx).await();
    CHECK(v1 == 42);
    CHECK(g_const_calls.load() == 1);

    auto v2 = s.evaluate_port<int64_t>(g, term, ctx).await();
    CHECK(v2 == 42);
    /* kernel must not have run again — value came from cache */
    CHECK(g_const_calls.load() == 1);
    CHECK(s.cache().hit_count()  >= 1);
    CHECK(s.cache().miss_count() >= 1);
}

TEST_CASE("OutputCache: time_invariant ignores ctx.time changes") {
    register_mock_kinds_clean();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1, .output_cache_capacity = 8},
                        frames, codecs);

    auto g = build_const_graph(7);
    graph::PortRef term{graph::NodeId{0}, 0};

    graph::EvalContext ctx_a{.time = {0, 1}};
    graph::EvalContext ctx_b{.time = {1, 1}};

    s.evaluate_port<int64_t>(g, term, ctx_a).await();
    s.evaluate_port<int64_t>(g, term, ctx_b).await();
    /* time is mixed into the cache key only when node.time_invariant
     * is false. const kernel marks itself invariant, so two distinct
     * times still cache-hit on the second call. */
    CHECK(g_const_calls.load() == 1);
}

TEST_CASE("OutputCache: time-dependent kernel re-runs on time change, hits on repeat") {
    register_mock_kinds_clean();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1, .output_cache_capacity = 8},
                        frames, codecs);

    auto g = build_time_dep_graph(3);
    graph::PortRef term{graph::NodeId{0}, 0};

    graph::EvalContext ctx_a{.time = {0, 1}};
    graph::EvalContext ctx_b{.time = {1, 1}};

    s.evaluate_port<int64_t>(g, term, ctx_a).await();
    s.evaluate_port<int64_t>(g, term, ctx_b).await();
    /* distinct times → distinct cache keys → kernel runs each time */
    CHECK(g_time_dep_calls.load() == 2);

    /* second call at ctx_a should hit */
    s.evaluate_port<int64_t>(g, term, ctx_a).await();
    CHECK(g_time_dep_calls.load() == 2);
}

TEST_CASE("OutputCache: capacity = 0 disables caching") {
    register_mock_kinds_clean();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1, .output_cache_capacity = 0},
                        frames, codecs);

    auto g = build_const_graph(99);
    graph::PortRef term{graph::NodeId{0}, 0};

    graph::EvalContext ctx;
    s.evaluate_port<int64_t>(g, term, ctx).await();
    s.evaluate_port<int64_t>(g, term, ctx).await();
    s.evaluate_port<int64_t>(g, term, ctx).await();
    CHECK(g_const_calls.load() == 3);
    CHECK(s.cache().hit_count() == 0);
}

TEST_CASE("OutputCache: bounded capacity evicts oldest LRU entry") {
    sched::OutputCache cache(2);

    using V = sched::OutputCache::Variant;
    cache.put(0xA, 0, V{int64_t{1}});
    cache.put(0xB, 0, V{int64_t{2}});
    CHECK(cache.size() == 2);

    /* Touch 0xA so it becomes most-recent; 0xB is now LRU. */
    auto a_hit = cache.get(0xA, 0);
    REQUIRE(a_hit.has_value());

    /* Insert a third entry → evicts the LRU (0xB). */
    cache.put(0xC, 0, V{int64_t{3}});
    CHECK(cache.size() == 2);

    CHECK(cache.get(0xB, 0).has_value() == false);
    CHECK(cache.get(0xA, 0).has_value() == true);
    CHECK(cache.get(0xC, 0).has_value() == true);
}

TEST_CASE("OutputCache: distinct port_idx are independent entries") {
    sched::OutputCache cache(8);
    using V = sched::OutputCache::Variant;
    cache.put(0xDEADBEEF, 0, V{int64_t{100}});
    cache.put(0xDEADBEEF, 1, V{int64_t{200}});
    auto p0 = cache.get(0xDEADBEEF, 0);
    auto p1 = cache.get(0xDEADBEEF, 1);
    REQUIRE(p0.has_value());
    REQUIRE(p1.has_value());
    CHECK(std::get<int64_t>(*p0) == 100);
    CHECK(std::get<int64_t>(*p1) == 200);
}
