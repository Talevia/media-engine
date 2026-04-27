/*
 * bench_thumbnail_png — throughput signal for `me_thumbnail_png`.
 *
 * me_thumbnail_png is one of M1's earliest C APIs (asset-level
 * thumbnail path in src/api/thumbnail.cpp, distinct from the
 * composition-level path that lives in
 * src/orchestrator/compose_frame.cpp::compose_png_at). Host scrub-row UIs
 * hit it at list-display cadence; a perf regression would surface
 * as janky scrubbing lists days / weeks after shipping.
 *
 * Synthesises nothing — reuses the determinism_fixture MP4 the
 * CMake test harness already builds. Iterates
 * me_thumbnail_png(uri, t=0.5s, 160×120) N times, reports avg ms
 * / fps, exits non-zero on budget miss. Skips (exit 0) when the
 * fixture isn't present or PNG encoder is unavailable.
 *
 * Budget: 25 ms avg per thumbnail on the dev box (~40 fps floor).
 * Standalone runs measure ~12 ms; the 2x headroom catches a >2x
 * regression while leaving room for ctest -j8 noise. RUN_SERIAL
 * additionally blocks co-scheduling with other tests, so the
 * shared-cache contention factor stays bounded. Tightened from
 * 50 ms in cycle 93 (debt-bench-thumbnail-budget-tightening).
 */
#include <media_engine.h>

#include "bench_harness.hpp"
#include "peak_rss.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr int    kIters       = 8;    /* total iterations */
constexpr int    kWarmup      = 2;    /* first N excluded from timing */
constexpr int    kOutWidth    = 160;
constexpr int    kOutHeight   = 120;
constexpr double kBudgetMs    = 25.0;  /* avg ms / thumbnail floor */

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_thumbnail_png <fixture-mp4>\n");
        return 1;
    }
    const std::string fixture_path = argv[1];
    if (!fs::exists(fixture_path)) {
        std::printf("bench_thumbnail_png: skipped (fixture not available: %s)\n",
                    fixture_path.c_str());
        return 0;
    }

    std::printf("bench_thumbnail_png: fixture=%s target=%dx%d iters=%d (warmup=%d) "
                "budget=%.1f ms/frame\n",
                fixture_path.c_str(), kOutWidth, kOutHeight, kIters, kWarmup,
                kBudgetMs);

    me_engine_t* eng = nullptr;
    if (me_engine_create(nullptr, &eng) != ME_OK) {
        std::fprintf(stderr, "bench_thumbnail_png: me_engine_create failed\n");
        return 1;
    }
    struct EngineGuard { me_engine_t* p; ~EngineGuard() { me_engine_destroy(p); } } g{eng};

    /* Sample peak RSS pre-workload so the post-workload reading lets
     * us report the *delta* the bench produced, not whatever the
     * process inherited from doctest harness / CI runner. */
    const std::int64_t rss_before = me::bench::peak_rss_bytes();

    const std::string uri = "file://" + fixture_path;
    const me_rational_t t = {1, 2};  /* ~0.5 s into the 2 s fixture */

    /* outcome lets the lambda short-circuit subsequent iterations
     * once the first call surfaces a skip / failure status; the
     * harness still spins through the remaining iterations but
     * each early-returns in <1 µs, well below noise. */
    me_status_t outcome = ME_OK;
    const double avg_sec = me::bench::measure_avg_sec(
        kIters, kWarmup, [&](int /*i*/) {
            if (outcome != ME_OK) return;
            uint8_t* png = nullptr;
            size_t   len = 0;
            outcome = me_thumbnail_png(eng, uri.c_str(), t,
                                        kOutWidth, kOutHeight,
                                        &png, &len);
            if (outcome == ME_OK) {
                if (!png || len == 0) outcome = ME_E_INTERNAL;
                else                  me_buffer_free(png);
            }
        });

    if (outcome == ME_E_UNSUPPORTED) {
        /* PNG encoder missing or source has no video stream —
         * no signal to emit. Skip with exit 0. */
        std::printf("bench_thumbnail_png: skipped (status=%d: %s)\n",
                    static_cast<int>(outcome), me_engine_last_error(eng));
        return 0;
    }
    if (outcome != ME_OK) {
        std::fprintf(stderr,
                     "bench_thumbnail_png: me_thumbnail_png failed (status=%d): %s\n",
                     static_cast<int>(outcome), me_engine_last_error(eng));
        return 1;
    }
    if (avg_sec <= 0.0) {
        std::fprintf(stderr, "bench_thumbnail_png: no timed iterations\n");
        return 1;
    }
    const double avg_ms = avg_sec * 1000.0;
    const double fps    = 1.0 / avg_sec;

    std::printf("bench_thumbnail_png: avg=%.3f ms (%.2f fps) budget=%.1f ms\n",
                avg_ms, fps, kBudgetMs);

    /* §5.7-1 cache stats dump — observation only here; numerical
     * lower-bound assertion lives in tests/test_cache_hit_rate_lower_bound.cpp.
     * Repeated me_thumbnail_png on the same (uri, t, w, h) should
     * exercise the asset-level decode cache, so post-warmup hits should
     * dominate. Visible in CI logs lets perf trend reviews catch a
     * cache-keying regression even if the timing budget still passes. */
    me_cache_stats_t cs{};
    if (me_cache_stats(eng, &cs) == ME_OK) {
        const std::int64_t total = cs.hit_count + cs.miss_count;
        const double hit_rate = total > 0
            ? static_cast<double>(cs.hit_count) / static_cast<double>(total)
            : 0.0;
        std::printf("bench_thumbnail_png: cache hits=%lld misses=%lld "
                    "hit_rate=%.2f entries=%lld mem=%lld B codec_ctx=%lld\n",
                    static_cast<long long>(cs.hit_count),
                    static_cast<long long>(cs.miss_count),
                    hit_rate,
                    static_cast<long long>(cs.entry_count),
                    static_cast<long long>(cs.memory_bytes_used),
                    static_cast<long long>(cs.codec_ctx_count));
    }

    /* §5.7-3 peak RSS — observation only (no budget). Establishes
     * the per-platform baseline; tightening into a hard budget is a
     * follow-up cycle once we have multi-run variance data. Reported
     * as delta over rss_before to subtract harness overhead. */
    const std::int64_t rss_after = me::bench::peak_rss_bytes();
    if (rss_before > 0 && rss_after > 0) {
        const std::int64_t delta = rss_after - rss_before;
        std::printf("bench_thumbnail_png: peak_rss before=%lld B after=%lld B "
                    "delta=%lld B\n",
                    static_cast<long long>(rss_before),
                    static_cast<long long>(rss_after),
                    static_cast<long long>(delta));
    } else {
        std::printf("bench_thumbnail_png: peak_rss unavailable on this platform\n");
    }

    if (avg_ms > kBudgetMs) {
        std::fprintf(stderr, "bench_thumbnail_png: PERF REGRESSION — "
                             "%.3f ms > %.1f ms budget\n",
                     avg_ms, kBudgetMs);
        return 1;
    }
    std::printf("bench_thumbnail_png: PASS\n");
    return 0;
}
