/*
 * test_bench_harness — doctest coverage for bench/bench_harness.hpp's
 * me::bench::measure_avg_sec template. Two consumers (bench_text_paragraph
 * + bench_thumbnail_png) exercise it transitively, but the corner cases
 * around the warmup contract (iters <= warmup → return 0.0) are not
 * surfaced by their happy-path runs. A regression that returned NaN or
 * a negative value would only show up as a budget miss and be hard to
 * diagnose. This suite pins the contract directly.
 */
#include <doctest/doctest.h>

#include "bench_harness.hpp"

#include <chrono>
#include <thread>

TEST_CASE("measure_avg_sec: iters > warmup runs work iters times, returns mean of post-warmup") {
    int call_count = 0;
    /* Use a very short sleep so the test stays under ~10 ms total
     * (8 iters × 1 ms = 8 ms body + ctest overhead). avg should
     * be close to 1 ms but the assertion only requires a positive
     * result — exact timing is jittery on shared CI hardware. */
    const auto sleep = std::chrono::microseconds(500);
    const double avg = me::bench::measure_avg_sec(
        /*iters=*/8, /*warmup=*/2, [&](int) {
            ++call_count;
            std::this_thread::sleep_for(sleep);
        });
    CHECK(call_count == 8);
    CHECK(avg > 0.0);
    /* Loose upper bound — a 500 µs sleep should never average above
     * 100 ms even under heavy contention. Catches a regression that
     * accidentally divided by 1 (returning total instead of mean). */
    CHECK(avg < 0.1);
}

TEST_CASE("measure_avg_sec: iters == warmup → 0.0 (no timed iterations)") {
    int call_count = 0;
    const double avg = me::bench::measure_avg_sec(
        /*iters=*/3, /*warmup=*/3, [&](int) { ++call_count; });
    /* Work still ran 3 times — warmup is timed but excluded from the
     * mean. Result is 0.0 because timed_n == 0. */
    CHECK(call_count == 3);
    CHECK(avg == doctest::Approx(0.0));
}

TEST_CASE("measure_avg_sec: iters < warmup → 0.0 (still iterates)") {
    int call_count = 0;
    const double avg = me::bench::measure_avg_sec(
        /*iters=*/2, /*warmup=*/5, [&](int) { ++call_count; });
    CHECK(call_count == 2);
    CHECK(avg == doctest::Approx(0.0));
}

TEST_CASE("measure_avg_sec: iters == 0 → 0.0 (work never invoked)") {
    int call_count = 0;
    const double avg = me::bench::measure_avg_sec(
        /*iters=*/0, /*warmup=*/0, [&](int) { ++call_count; });
    CHECK(call_count == 0);
    CHECK(avg == doctest::Approx(0.0));
}

TEST_CASE("measure_avg_sec: iteration index is 0..iters-1, monotonically increasing") {
    /* Confirms the documented contract that work(i) gets sequential
     * indices starting at 0. A regression that started at 1 or
     * shuffled indices would break benches that key per-iteration
     * setup off `i`. */
    int last_seen  = -1;
    int max_seen   = -1;
    bool monotonic = true;
    me::bench::measure_avg_sec(
        /*iters=*/5, /*warmup=*/0, [&](int i) {
            if (i <= last_seen) monotonic = false;
            last_seen = i;
            if (i > max_seen) max_seen = i;
        });
    CHECK(monotonic);
    CHECK(max_seen == 4);  /* 0..4 inclusive */
}
