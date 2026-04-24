/*
 * me::bench::measure_avg_sec — shared timing harness for bench/.
 *
 * Header-only template. Replaces the warmup + steady_clock +
 * accumulate + divide ritual that bench_text_paragraph and
 * bench_thumbnail_png each rolled by hand. Adding a new timed
 * bench is now one call — no chrono boilerplate.
 *
 * Contract:
 *   - Calls `work(i)` for `iters` iterations (i = 0..iters-1).
 *   - First `warmup` iterations are timed but excluded from the
 *     reported average (primes Skia's glyph cache, FFmpeg's
 *     codec lookup tables, OS page cache, etc.).
 *   - Returns the average wall-clock seconds per *post-warmup*
 *     iteration. `iters > warmup` is the caller's contract; if
 *     not, returns 0.0 (caller can detect and bail).
 *
 * Why steady_clock: monotonic; immune to wall-clock jumps
 * (NTP step, daylight savings) that would corrupt a single
 * sample. system_clock is not safe.
 *
 * Why expose `iters` + `warmup` as runtime args: bench targets
 * tune them per-workload (8/2 for fast renders; 200/50 for
 * sub-millisecond ops where measurement noise dominates one
 * call). Compile-time constants would force one shape.
 *
 * Skipped benches: when the workload can't run (codec missing,
 * fixture absent), the bench's main() returns 0 *before* calling
 * this helper — see bench_thumbnail_png's early-return on
 * ME_E_UNSUPPORTED. The harness has no opinion about skipping.
 */
#pragma once

#include <chrono>

namespace me::bench {

/* Run `work(i)` `iters` times; return the mean wall-clock
 * second per post-warmup iteration. Returns 0.0 if no timed
 * iterations ran (iters <= warmup). */
template <typename Work>
double measure_avg_sec(int iters, int warmup, Work work) {
    auto total_timed = std::chrono::duration<double>::zero();
    int timed_n = 0;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        work(i);
        const auto t1 = std::chrono::steady_clock::now();
        if (i >= warmup) {
            total_timed += (t1 - t0);
            ++timed_n;
        }
    }
    if (timed_n <= 0) return 0.0;
    return total_timed.count() / static_cast<double>(timed_n);
}

}  // namespace me::bench
