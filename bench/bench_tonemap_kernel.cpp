/*
 * bench_tonemap_kernel — full-frame 4K throughput signal for
 * `me::compose::apply_tonemap_inplace` (M10 HDR→SDR ship-path
 * landed cycle 9). Per the bullet `bench-perf-budget-coverage-expand`,
 * M10 added several hot paths that lacked bench coverage; tonemap
 * is the most-likely-to-regress one because it runs per-pixel per-
 * frame inside the compose chain whenever an HDR source feeds an
 * SDR output (a common production scenario).
 *
 * Synthesises a 3840×2160 RGBA8 buffer (~31.6 MB) populated with a
 * deterministic gradient that exercises both the bright (highlight
 * compression) and dark (shadow lift) parts of the Hable curve,
 * then iterates `apply_tonemap_inplace` N times under the standard
 * harness pattern. Reports avg ms / Mpix per second, exits non-
 * zero on budget miss.
 *
 * Budget: 400 ms avg per 4K frame on the dev box. Standalone runs
 * measure ~195 ms (≈ 42 Mpix/s scalar Hable on a stock x86_64
 * Mac). The 2× headroom catches a >2× regression while leaving
 * room for ctest -j8 noise — same convention as the other benches
 * (bench_thumbnail_png 25 ms vs ~12 ms observed). The 4K-at-200ms
 * baseline ALSO documents that 4K tonemap is not real-time at
 * 24+ fps without SIMD; that's a future optimisation cycle, not
 * a regression to catch here. RUN_SERIAL blocks co-scheduling
 * with other CPU-bound benches.
 *
 * Why no fixture: the kernel is pure-byte math (no FFmpeg, no
 * file I/O). A synthetic gradient is sufficient + deterministic +
 * ~15× faster to set up than decoding a frame.
 */
#include "bench_harness.hpp"
#include "compose/tonemap_kernel.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int    kWidth    = 3840;
constexpr int    kHeight   = 2160;
constexpr int    kIters    = 6;     /* total iterations */
constexpr int    kWarmup   = 2;     /* first N excluded from timing */
constexpr double kBudgetMs = 400.0;  /* avg ms / 4K frame floor (~42 Mpix/s baseline × 2 headroom) */

/* Deterministic gradient that exercises bright + dark Hable
 * branches. Each pixel's R/G/B = (x + y + i) % 256 → covers the
 * full byte range across the frame; alpha = 255 (opaque). */
void fill_gradient(std::vector<std::uint8_t>& buf, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            const std::uint8_t v = static_cast<std::uint8_t>((x + y) & 0xff);
            buf[i + 0] = v;
            buf[i + 1] = v;
            buf[i + 2] = v;
            buf[i + 3] = 255;
        }
    }
}

}  // namespace

int main() {
    std::printf("bench_tonemap_kernel: target=%dx%d iters=%d (warmup=%d) "
                "budget=%.1f ms/frame\n",
                kWidth, kHeight, kIters, kWarmup, kBudgetMs);

    const std::size_t buf_bytes =
        static_cast<std::size_t>(kWidth) * kHeight * 4;
    std::vector<std::uint8_t> buf(buf_bytes);

    me::TonemapEffectParams params;
    params.algo        = me::TonemapEffectParams::Algo::Hable;
    params.target_nits = 100.0;   /* SDR target (1000 nits HDR → 100 nits SDR). */

    me_status_t outcome = ME_OK;
    const double avg_sec = me::bench::measure_avg_sec(
        kIters, kWarmup, [&](int /*i*/) {
            if (outcome != ME_OK) return;
            /* Refresh the gradient each iteration so the kernel sees
             * a fresh pixel distribution every call (tonemap is
             * in-place; without re-fill, iter N would see iter
             * N-1's output and saturate after the first call). */
            fill_gradient(buf, kWidth, kHeight);
            outcome = me::compose::apply_tonemap_inplace(
                buf.data(), kWidth, kHeight,
                static_cast<std::size_t>(kWidth) * 4, params);
        });

    if (outcome != ME_OK) {
        std::fprintf(stderr,
                     "bench_tonemap_kernel: apply_tonemap_inplace failed (status=%d)\n",
                     static_cast<int>(outcome));
        return 1;
    }
    if (avg_sec <= 0.0) {
        std::fprintf(stderr, "bench_tonemap_kernel: no timed iterations\n");
        return 1;
    }

    const double avg_ms      = avg_sec * 1000.0;
    const double frame_pix   = static_cast<double>(kWidth) * kHeight;
    const double mpix_per_s  = frame_pix / 1e6 / avg_sec;

    std::printf("bench_tonemap_kernel: avg=%.3f ms (%.1f Mpix/s) budget=%.1f ms\n",
                avg_ms, mpix_per_s, kBudgetMs);

    if (avg_ms > kBudgetMs) {
        std::fprintf(stderr,
                     "bench_tonemap_kernel: BUDGET MISS — %.3f ms > %.1f ms\n",
                     avg_ms, kBudgetMs);
        return 1;
    }
    std::printf("bench_tonemap_kernel: PASS\n");
    return 0;
}
