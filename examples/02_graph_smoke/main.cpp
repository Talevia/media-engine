/*
 * 02_graph_smoke — proves graph to task to scheduler to Future path works end-to-end.
 *
 * This example reaches into the engine's INTERNAL headers under
 * src/graph, src/task, src/scheduler — not the public C API. That's
 * intentional: it's an engine-internal validation tool, not an API demo.
 * Public users should use media_engine.h via orchestrators (coming soon).
 *
 * Test scenarios:
 *   1. Single-node graph: TestConstInt(value=42) → expect 42
 *   2. Diamond graph: const(3) + const(4) via TestAddInt → expect 7
 *   3. Deterministic content_hash: same graph twice → same hash
 *   4. Terminal name lookup
 *   5. Error propagation: kernel returns non-OK → await throws
 */
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "resource/frame_pool.hpp"
#include "scheduler/scheduler.hpp"
#include "task/context.hpp"
#include "task/registry.hpp"
#include "task/task_kind.hpp"

#include <cassert>
#include <cstdio>
#include <span>
#include <string>

using namespace me;

namespace {

me_status_t kernel_const_int(task::TaskContext&,
                             const graph::Properties& props,
                             std::span<const graph::InputValue>,
                             std::span<graph::OutputSlot> outs) {
    auto it = props.find("value");
    if (it == props.end()) return ME_E_INVALID_ARG;
    outs[0].v = std::get<int64_t>(it->second.v);
    return ME_OK;
}

me_status_t kernel_add_int(task::TaskContext&,
                           const graph::Properties&,
                           std::span<const graph::InputValue> ins,
                           std::span<graph::OutputSlot>       outs) {
    const int64_t a = std::get<int64_t>(ins[0].v);
    const int64_t b = std::get<int64_t>(ins[1].v);
    outs[0].v = int64_t{a + b};
    return ME_OK;
}

me_status_t kernel_always_fail(task::TaskContext&,
                               const graph::Properties&,
                               std::span<const graph::InputValue>,
                               std::span<graph::OutputSlot>) {
    return ME_E_INTERNAL;
}

void register_test_kinds() {
    task::reset_registry_for_testing();

    task::KindInfo k_const{
        .kind = task::TaskKindId::TestConstInt,
        .affinity = task::Affinity::Cpu,
        .latency  = task::Latency::Short,
        .time_invariant = true,
        .kernel = kernel_const_int,
        .input_schema  = {},
        .output_schema = { {"value", graph::TypeId::Int64} },
        .param_schema  = {
            {.name = "value", .type = graph::TypeId::Int64 }
        },
    };
    task::register_kind(k_const);

    task::KindInfo k_add{
        .kind = task::TaskKindId::TestAddInt,
        .affinity = task::Affinity::Cpu,
        .latency  = task::Latency::Short,
        .time_invariant = true,
        .kernel = kernel_add_int,
        .input_schema  = {
            {"a", graph::TypeId::Int64},
            {"b", graph::TypeId::Int64}
        },
        .output_schema = { {"sum", graph::TypeId::Int64} },
        .param_schema  = {},
    };
    task::register_kind(k_add);

    task::KindInfo k_fail{
        .kind = task::TaskKindId::TestEchoString,  /* reusing slot for a fail case */
        .affinity = task::Affinity::Cpu,
        .kernel = kernel_always_fail,
        .input_schema  = {},
        .output_schema = { {"never", graph::TypeId::Int64} },
    };
    task::register_kind(k_fail);
}

int test_single_node() {
    register_test_kinds();

    graph::Graph::Builder b;
    graph::Properties props;
    props["value"].v = int64_t{42};
    auto n = b.add(task::TaskKindId::TestConstInt, std::move(props), {});
    b.name_terminal("value", graph::PortRef{n, 0});
    graph::Graph g = std::move(b).build();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 2}, frames, codecs);

    graph::EvalContext ctx;
    ctx.frames = &frames;
    auto fut = s.evaluate_port<int64_t>(g, graph::PortRef{n, 0}, ctx);
    int64_t result = fut.await();

    if (result != 42) { std::fprintf(stderr, "single_node: got %lld\n", (long long)result); return 1; }
    std::fprintf(stderr, "  test_single_node: OK (42)\n");
    return 0;
}

int test_diamond_add() {
    register_test_kinds();

    graph::Graph::Builder b;
    graph::Properties p3;  p3["value"].v = int64_t{3};
    graph::Properties p4;  p4["value"].v = int64_t{4};
    auto a = b.add(task::TaskKindId::TestConstInt, std::move(p3), {});
    auto c = b.add(task::TaskKindId::TestConstInt, std::move(p4), {});
    auto sum = b.add(task::TaskKindId::TestAddInt, {}, {graph::PortRef{a,0}, graph::PortRef{c,0}});
    b.name_terminal("out", graph::PortRef{sum, 0});
    graph::Graph g = std::move(b).build();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 2}, frames, codecs);

    graph::EvalContext ctx;
    auto fut = s.evaluate_port<int64_t>(g, graph::PortRef{sum, 0}, ctx);
    int64_t result = fut.await();

    if (result != 7) { std::fprintf(stderr, "diamond_add: got %lld\n", (long long)result); return 1; }
    std::fprintf(stderr, "  test_diamond_add: OK (3+4=7)\n");
    return 0;
}

int test_deterministic_hash() {
    register_test_kinds();

    auto build_same = []() {
        graph::Graph::Builder b;
        graph::Properties p;  p["value"].v = int64_t{100};
        b.add(task::TaskKindId::TestConstInt, std::move(p), {});
        return std::move(b).build();
    };
    auto g1 = build_same();
    auto g2 = build_same();
    if (g1.content_hash() != g2.content_hash()) {
        std::fprintf(stderr, "hash mismatch: %llx vs %llx\n",
            (unsigned long long)g1.content_hash(),
            (unsigned long long)g2.content_hash());
        return 1;
    }
    std::fprintf(stderr, "  test_deterministic_hash: OK (%llx)\n",
        (unsigned long long)g1.content_hash());
    return 0;
}

int test_terminal_lookup() {
    register_test_kinds();

    graph::Graph::Builder b;
    graph::Properties p;  p["value"].v = int64_t{1};
    auto n = b.add(task::TaskKindId::TestConstInt, std::move(p), {});
    b.name_terminal("video", graph::PortRef{n, 0});
    b.name_terminal("audio", graph::PortRef{n, 0});
    graph::Graph g = std::move(b).build();

    if (!g.terminal("video").has_value()) return 1;
    if (!g.terminal("audio").has_value()) return 1;
    if (g.terminal("missing").has_value()) return 1;
    std::fprintf(stderr, "  test_terminal_lookup: OK (2 terminals)\n");
    return 0;
}

int test_error_propagation() {
    register_test_kinds();

    graph::Graph::Builder b;
    auto n = b.add(task::TaskKindId::TestEchoString, {}, {});  /* fail kernel */
    graph::Graph g = std::move(b).build();

    resource::FramePool frames;
    resource::CodecPool codecs;
    sched::Scheduler s({.cpu_threads = 1}, frames, codecs);

    graph::EvalContext ctx;
    auto fut = s.evaluate_port<int64_t>(g, graph::PortRef{n, 0}, ctx);
    try {
        fut.await();
        std::fprintf(stderr, "error_propagation: expected throw, got silence\n");
        return 1;
    } catch (const std::exception&) {
        std::fprintf(stderr, "  test_error_propagation: OK (threw as expected)\n");
        return 0;
    }
}

}  // namespace

int main() {
    std::fprintf(stderr, "02_graph_smoke — graph-task-bootstrap validation\n");
    int rc = 0;
    rc |= test_single_node();
    rc |= test_diamond_add();
    rc |= test_deterministic_hash();
    rc |= test_terminal_lookup();
    rc |= test_error_propagation();
    std::fprintf(stderr, rc == 0 ? "ALL PASS\n" : "FAIL\n");
    return rc;
}
