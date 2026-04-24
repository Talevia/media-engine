/*
 * bench_text_paragraph — throughput signal for
 * SkiaBackend::draw_paragraph (text-clip-multiline-word-wrap,
 * cycle 50 4677e21).
 *
 * Synthesises a ~1000-codepoint mixed content string (CJK + Latin
 * + emoji), renders it N times through TextRenderer with
 * max_width set, then reports the measured fps.
 *
 * Budget: 60 fps for a 1000-codepoint paragraph on the dev box.
 * Dev hardware today renders a ~20-codepoint paragraph in a
 * fraction of a millisecond; 1000 codepoints should complete well
 * under 16 ms, so 60 fps is a loose floor that catches a 10×
 * regression but tolerates normal measurement noise.
 *
 * Exit code: 0 = pass, 1 = budget miss, 0 = skipped (Skia build
 * unavailable — matches the pattern other benches use).
 */
#ifdef ME_HAS_SKIA

#include "text/text_renderer.hpp"
#include "timeline/animated_color.hpp"
#include "timeline/animated_number.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kCanvasW     = 720;
constexpr int kCanvasH     = 480;
constexpr int kIters       = 8;   /* warm-up + timed runs */
constexpr int kWarmup      = 2;   /* first N excluded from the timing */
constexpr int kTargetCps   = 1000;
constexpr int kFpsBudget   = 60;

/* Build a ~`kTargetCps`-codepoint string that mixes scripts the
 * greedy codepoint wrap must handle: CJK (variable-width UTF-8),
 * Latin words with whitespace, and a few emoji that trigger
 * SkFontMgr::matchFamilyStyleCharacter fallbacks. */
std::string make_long_content() {
    const std::string unit =
        "你好世界 Hello World 日本語 "
        "中国語 \xF0\x9F\x8E\x89 emoji caption 123 "
        "motion graphics テスト ";
    /* Measure codepoints in `unit` — CJK chars are 3 bytes, emoji
     * are 4 bytes. For a coarse ~1000-codepoint target, just
     * repeat until the buffer's byte length crosses a rough
     * multiplier. */
    std::string out;
    out.reserve(kTargetCps * 4);
    while (static_cast<int>(out.size()) < kTargetCps * 3) {
        out += unit;
    }
    return out;
}

}  // namespace

int main() {
    std::printf("bench_text_paragraph: canvas=%dx%d iters=%d (warmup=%d) "
                "target_cps=%d fps_budget=%d\n",
                kCanvasW, kCanvasH, kIters, kWarmup, kTargetCps, kFpsBudget);

    me::text::TextRenderer r(kCanvasW, kCanvasH);
    if (!r.valid()) {
        std::printf("bench_text_paragraph: skipped (SkiaBackend invalid; "
                    "ctor failed at runtime)\n");
        return 0;
    }

    me::TextClipParams params;
    params.content                = make_long_content();
    params.color                  = me::AnimatedColor::from_hex("#FFFFFFFF");
    params.font_size              = me::AnimatedNumber::from_static(24.0);
    params.x                      = me::AnimatedNumber::from_static(4.0);
    params.y                      = me::AnimatedNumber::from_static(32.0);
    params.max_width              = static_cast<double>(kCanvasW - 8);
    params.line_height_multiplier = 1.2;

    const std::size_t stride = static_cast<std::size_t>(kCanvasW) * 4;
    std::vector<std::uint8_t> buf(
        static_cast<std::size_t>(kCanvasH) * stride, 0);

    /* Warm-up iterations: first draw primes Skia's glyph cache +
     * font-fallback tables. Only post-warm-up iterations count
     * toward the fps calculation. */
    auto total_timed = std::chrono::duration<double>::zero();
    int timed_n = 0;
    for (int i = 0; i < kIters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        r.render(params, me_rational_t{i, 1}, buf.data(), stride);
        const auto t1 = std::chrono::steady_clock::now();
        if (i >= kWarmup) {
            total_timed += (t1 - t0);
            ++timed_n;
        }
    }

    if (timed_n <= 0) {
        std::fprintf(stderr, "bench_text_paragraph: no timed iterations\n");
        return 1;
    }
    const double avg_sec = total_timed.count() / timed_n;
    const double fps = 1.0 / avg_sec;

    std::printf("bench_text_paragraph: content_bytes=%zu avg=%.3f ms "
                "fps=%.2f budget=%d\n",
                params.content.size(), avg_sec * 1000.0, fps, kFpsBudget);

    if (fps < kFpsBudget) {
        std::fprintf(stderr, "bench_text_paragraph: PERF REGRESSION — "
                             "fps %.2f below budget %d\n",
                     fps, kFpsBudget);
        return 1;
    }
    std::printf("bench_text_paragraph: PASS\n");
    return 0;
}

#else  /* !ME_HAS_SKIA */

#include <cstdio>

int main() {
    std::printf("bench_text_paragraph: skipped (ME_WITH_SKIA=OFF)\n");
    return 0;
}

#endif  /* ME_HAS_SKIA */
