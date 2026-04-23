/*
 * taskflow_probe — verifies Taskflow compiles and links with our toolchain.
 *
 * This is a placeholder that the `graph-task-bootstrap` backlog will replace
 * with a real `scheduler.cpp` using `tf::Executor`. The probe runs a tiny
 * 3-node DAG once at library load to exercise the work-stealing path so any
 * ABI / version mismatch surfaces at startup rather than first render.
 */
#include <taskflow/taskflow.hpp>

#include <atomic>

namespace me::sched::detail {

namespace {

// Run a three-node diamond: A fans out to {B, C}, both sink into D.
// Anything compiling means the header is usable; running on ctor proves the
// executor works before any real task is submitted.
int run_probe() {
    tf::Executor executor(2);
    tf::Taskflow flow;
    std::atomic<int> counter{0};

    auto A = flow.emplace([&] { counter.fetch_add(1); });
    auto B = flow.emplace([&] { counter.fetch_add(10); });
    auto C = flow.emplace([&] { counter.fetch_add(100); });
    auto D = flow.emplace([&] { counter.fetch_add(1000); });
    A.precede(B, C);
    B.precede(D);
    C.precede(D);

    executor.run(flow).wait();
    return counter.load();  // expected 1111
}

// Consume the result so the compiler can't elide the call.
[[maybe_unused]] const int probe_result = run_probe();

}  // namespace

// Re-exposed so a caller can assert-on-startup that the probe ran.
int taskflow_probe_result() { return probe_result; }

}  // namespace me::sched::detail
