/*
 * bench_audio_effect_chain — per-chunk throughput signal for
 * `me::audio::AudioEffectChain` (M4 audio effect chain;
 * Gain / Pan / Lowpass landed pre-M10, PeakingEq landed cycle 22).
 * Per the bullet `bench-audio-effect-chain`, the chain processes
 * audio in `Player::audio_producer_loop`'s per-chunk emit path; a
 * regression in any effect would surface as audio underruns under
 * pacer pressure, not as a test failure.
 *
 * Synthesises a 1024-frame × 48 kHz × stereo float buffer (i.e.
 * one canonical AAC-frame-sized chunk) populated with a
 * deterministic interleaved sine, builds an AudioEffectChain
 * with all four phase-1 effect types (Gain → Pan → PeakingEq →
 * Lowpass), iterates `chain.process` N times under the standard
 * harness pattern. Reports avg µs / chunk + chunks-per-second
 * throughput, exits non-zero on budget miss.
 *
 * Why all four effects: matches the most-extended use case (a
 * full per-clip audio chain). A regression in any single effect
 * shows up here; isolating which one is the test_audio_effect_chain
 * suite's job.
 *
 * Budget: 0.2 ms / chunk on the dev box. Standalone measures
 * 26..31 µs across runs (≈ 33k chunks/s — well above real-time
 * 47 chunks/s for 48 kHz × 1024-frame chunks). The ~6.5×
 * headroom from the observed upper bound catches a >4×
 * regression while leaving room for ctest -j8 scheduling jitter
 * (single-OS-slice preempts can cost 50-100 µs and the harness
 * averages over 150 timed iters). Slightly looser than the
 * canonical 2× convention because sub-millisecond op timing is
 * inherently noisier than the ms-scale benches.
 */
#include "audio/audio_effect_chain.hpp"
#include "audio/gain_audio_effect.hpp"
#include "audio/lowpass_audio_effect.hpp"
#include "audio/pan_audio_effect.hpp"
#include "audio/peaking_eq_audio_effect.hpp"
#include "bench_harness.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int    kSampleRate   = 48000;
constexpr int    kChannels     = 2;
constexpr int    kFramesPerChunk = 1024;   /* canonical AAC frame size */
constexpr int    kIters        = 200;       /* short chunks → many iters for stable mean */
constexpr int    kWarmup       = 50;
constexpr double kBudgetMs     = 0.2;       /* avg ms / chunk floor (~6.5× headroom over 31µs observed upper) */

constexpr double kPi = 3.14159265358979323846;

/* Fill an interleaved stereo buffer with a 440 Hz sine on L,
 * 220 Hz on R. Mirrors test_audio_effect_chain's signal shape
 * so the bench works the same code-paths the regression suite
 * does. */
void fill_sine(std::vector<float>& buf, int sample_rate, int n_frames) {
    for (int i = 0; i < n_frames; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        buf[i * 2 + 0] = static_cast<float>(std::sin(2.0 * kPi * 440.0 * t));
        buf[i * 2 + 1] = static_cast<float>(std::sin(2.0 * kPi * 220.0 * t));
    }
}

}  // namespace

int main() {
    std::printf("bench_audio_effect_chain: chunk=%d frames @ %d Hz × %d ch  "
                "iters=%d (warmup=%d) budget=%.1f ms/chunk\n",
                kFramesPerChunk, kSampleRate, kChannels,
                kIters, kWarmup, kBudgetMs);

    me::audio::AudioEffectChain chain;
    chain.append(std::make_unique<me::audio::GainAudioEffect>(0.8f));
    chain.append(std::make_unique<me::audio::PanAudioEffect>(0.2f));
    chain.append(std::make_unique<me::audio::PeakingEqAudioEffect>(
        /*freq_hz=*/1000.0f, /*gain_db=*/+6.0f, /*q=*/1.4f));
    chain.append(std::make_unique<me::audio::LowpassAudioEffect>(
        /*cutoff_hz=*/8000.0f));

    std::vector<float> buf(static_cast<std::size_t>(kFramesPerChunk) * kChannels);

    const double avg_sec = me::bench::measure_avg_sec(
        kIters, kWarmup, [&](int /*i*/) {
            /* Refresh the input each iteration — Lowpass + PeakingEq
             * are stateful; without re-fill the filter state would
             * drift towards a steady state and timing would
             * skew low after a few iterations. */
            fill_sine(buf, kSampleRate, kFramesPerChunk);
            chain.process(buf.data(), kFramesPerChunk, kChannels, kSampleRate);
        });

    if (avg_sec <= 0.0) {
        std::fprintf(stderr, "bench_audio_effect_chain: no timed iterations\n");
        return 1;
    }

    const double avg_ms        = avg_sec * 1000.0;
    const double avg_us        = avg_sec * 1e6;
    const double chunks_per_s  = 1.0 / avg_sec;

    std::printf("bench_audio_effect_chain: avg=%.3f ms (%.0f µs, %.0f chunks/s) "
                "budget=%.1f ms\n",
                avg_ms, avg_us, chunks_per_s, kBudgetMs);

    if (avg_ms > kBudgetMs) {
        std::fprintf(stderr,
                     "bench_audio_effect_chain: BUDGET MISS — %.3f ms > %.1f ms\n",
                     avg_ms, kBudgetMs);
        return 1;
    }
    std::printf("bench_audio_effect_chain: PASS\n");
    return 0;
}
