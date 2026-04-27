/*
 * bench_ocio_apply — full-frame 4K throughput signal for
 * `me::color::OcioPipeline::apply` (M2 OCIO color pipeline +
 * cycle 6's ocio-config-env-override). Per the bullet
 * `bench-ocio-apply-cost`, the per-frame OCIO apply cost is on
 * the compose path whenever colorSpace ≠ identity (= almost every
 * non-bt709-only timeline) but lacked bench coverage.
 *
 * Synthesises a 3840×2160 RGBA8 buffer populated with a deterministic
 * gradient + iterates `apply(bt709 → linear)` N times. Reports avg
 * ms / Mpix per second, exits non-zero on budget miss. Skipped
 * (exit 0) when ME_WITH_OCIO=OFF — the build-time gate flips this
 * file out via `target_compile_definitions`-driven `#ifdef`.
 *
 * Choice of transform: bt709 → linear hits the canonical un-gamma
 * pass (decode the BT.1886 video EOTF into scene-linear). Same
 * supported subset that to_ocio_name in src/color/ocio_pipeline.cpp
 * exposes today (bt709 / sRGB / linear). bt2020 / PQ aren't in the
 * built-in OCIO config phase-1 supports; using them here would
 * fall straight to ME_E_UNSUPPORTED and yield no signal.
 *
 * Budget: 150 ms / 4K frame on the dev box. Standalone runs measure
 * ~74 ms (≈ 112 Mpix/s for OCIO's uint8-optimized CPU path bt709 →
 * linear via the built-in cg-config + OPTIMIZATION_DEFAULT). The 2×
 * headroom convention from bench_tonemap_kernel.cpp (cycle 31) applies
 * — catches a >2× regression while leaving room for ctest -j8 noise
 * + OCIO LUT-building variance across config versions.
 */
#ifndef ME_HAS_OCIO

#include <cstdio>

int main() {
    std::printf("bench_ocio_apply: skipped (ME_WITH_OCIO=OFF — no OcioPipeline to bench)\n");
    return 0;
}

#else

#include "bench_harness.hpp"
#include "color/pipeline.hpp"
#include "timeline/timeline_impl.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int    kWidth    = 3840;
constexpr int    kHeight   = 2160;
constexpr int    kIters    = 6;
constexpr int    kWarmup   = 2;
constexpr double kBudgetMs = 150.0;   /* avg ms / 4K frame floor (~112 Mpix/s baseline × 2 headroom) */

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
    std::printf("bench_ocio_apply: target=%dx%d iters=%d (warmup=%d) "
                "budget=%.1f ms/frame\n",
                kWidth, kHeight, kIters, kWarmup, kBudgetMs);

    auto pipeline = me::color::make_pipeline(/*config_path=*/nullptr);
    if (!pipeline) {
        std::fprintf(stderr, "bench_ocio_apply: make_pipeline returned null\n");
        return 1;
    }

    me::ColorSpace src_cs;
    src_cs.primaries = me::ColorSpace::Primaries::BT709;
    src_cs.transfer  = me::ColorSpace::Transfer::BT709;
    src_cs.matrix    = me::ColorSpace::Matrix::BT709;
    src_cs.range     = me::ColorSpace::Range::Limited;

    me::ColorSpace dst_cs = src_cs;
    dst_cs.transfer  = me::ColorSpace::Transfer::Linear;
    dst_cs.range     = me::ColorSpace::Range::Full;

    const std::size_t buf_bytes =
        static_cast<std::size_t>(kWidth) * kHeight * 4;
    std::vector<std::uint8_t> buf(buf_bytes);

    me_status_t outcome = ME_OK;
    std::string err;
    const double avg_sec = me::bench::measure_avg_sec(
        kIters, kWarmup, [&](int /*i*/) {
            if (outcome != ME_OK) return;
            /* Refresh the gradient each iteration — apply mutates
             * in-place; without re-fill, iter 2 would see iter 1's
             * already-linearized output and the result drifts away
             * from the bench's intent (timing the bt709→linear
             * transform, not a chain of transforms). */
            fill_gradient(buf, kWidth, kHeight);
            outcome = pipeline->apply(buf.data(), buf_bytes, src_cs, dst_cs, &err);
        });

    if (outcome == ME_E_UNSUPPORTED) {
        /* IdentityPipeline returns ME_OK on every input; if we hit
         * UNSUPPORTED here the build linked OcioPipeline but the
         * src/dst pair doesn't map (`to_ocio_name` returned nullptr).
         * Skip with exit 0 — the bench has no signal to report but
         * neither is the engine broken. */
        std::printf("bench_ocio_apply: skipped (status=%d: %s)\n",
                    static_cast<int>(outcome), err.c_str());
        return 0;
    }
    if (outcome != ME_OK) {
        std::fprintf(stderr,
                     "bench_ocio_apply: OcioPipeline::apply failed (status=%d): %s\n",
                     static_cast<int>(outcome), err.c_str());
        return 1;
    }
    if (avg_sec <= 0.0) {
        std::fprintf(stderr, "bench_ocio_apply: no timed iterations\n");
        return 1;
    }

    const double avg_ms      = avg_sec * 1000.0;
    const double frame_pix   = static_cast<double>(kWidth) * kHeight;
    const double mpix_per_s  = frame_pix / 1e6 / avg_sec;

    std::printf("bench_ocio_apply: avg=%.3f ms (%.1f Mpix/s) budget=%.1f ms\n",
                avg_ms, mpix_per_s, kBudgetMs);

    if (avg_ms > kBudgetMs) {
        std::fprintf(stderr,
                     "bench_ocio_apply: BUDGET MISS — %.3f ms > %.1f ms\n",
                     avg_ms, kBudgetMs);
        return 1;
    }
    std::printf("bench_ocio_apply: PASS\n");
    return 0;
}

#endif /* ME_HAS_OCIO */
