/*
 * test_tempo — pins me::audio::TempoStretcher's SoundTouch
 * wrapping. Proves:
 *   - tempo=1.0 passes input through essentially unchanged (up
 *     to SoundTouch's internal latency trimming).
 *   - tempo=2.0 produces roughly half the output frames for the
 *     same input.
 *   - tempo=2.0 preserves pitch — zero-crossing count of a pure
 *     sine wave stays proportional to the input frequency, not
 *     the input-over-output ratio.
 *
 * Only built when ME_WITH_SOUNDTOUCH=ON (ME_HAS_SOUNDTOUCH compile
 * def).
 */
#ifdef ME_HAS_SOUNDTOUCH

#include <doctest/doctest.h>

#include "audio/tempo.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

/* Generate a pure sine wave at `freq_hz` for `duration_seconds`
 * at `sample_rate`. Mono. */
std::vector<float> make_sine(int sample_rate, double freq_hz,
                              double duration_seconds) {
    const std::size_t n = static_cast<std::size_t>(
        static_cast<double>(sample_rate) * duration_seconds);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        out[i] = static_cast<float>(std::sin(2.0 * kPi * freq_hz * t));
    }
    return out;
}

/* Count zero crossings — changes of sign in a signal. For a pure
 * sine at frequency F, the crossing count over duration D seconds
 * is approximately 2 * F * D. We use this to verify pitch is
 * preserved: tempo stretches duration but crossings stay
 * proportional to input F (not input-F × tempo-ratio). */
int count_zero_crossings(const std::vector<float>& samples) {
    if (samples.size() < 2) return 0;
    int count = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const bool prev_pos = samples[i - 1] >= 0.0f;
        const bool curr_pos = samples[i]     >= 0.0f;
        if (prev_pos != curr_pos) ++count;
    }
    return count;
}

/* Drive the stretcher end-to-end: put all input, flush, pull all
 * output. Returns the interleaved output samples. Mono case
 * (channels=1) for simplicity. */
std::vector<float> stretch_mono(const std::vector<float>& input,
                                 int sample_rate, double tempo) {
    me::audio::TempoStretcher st(sample_rate, /*channels=*/1);
    st.set_tempo(tempo);
    st.put_samples(input.data(), input.size());
    st.flush();

    std::vector<float> out;
    float             buf[4096];
    for (;;) {
        const std::size_t got = st.receive_samples(buf,
            sizeof(buf) / sizeof(buf[0]));
        if (got == 0) break;
        out.insert(out.end(), buf, buf + got);
    }
    return out;
}

}  // namespace

TEST_CASE("TempoStretcher: tempo=1.0 preserves length approximately") {
    constexpr int SR       = 44100;
    constexpr double FREQ  = 440.0;
    constexpr double DUR_S = 0.5;

    const auto in   = make_sine(SR, FREQ, DUR_S);
    const auto out  = stretch_mono(in, SR, 1.0);

    /* SoundTouch drops some samples at the start for its internal
     * look-ahead buffer + trims the tail. Allow 10% slack. */
    const double ratio = static_cast<double>(out.size()) /
                         static_cast<double>(in.size());
    CHECK(ratio > 0.9);
    CHECK(ratio < 1.1);
}

TEST_CASE("TempoStretcher: tempo=2.0 halves output length") {
    constexpr int SR       = 44100;
    constexpr double FREQ  = 440.0;
    constexpr double DUR_S = 1.0;

    const auto in   = make_sine(SR, FREQ, DUR_S);
    const auto out  = stretch_mono(in, SR, 2.0);

    const double ratio = static_cast<double>(out.size()) /
                         static_cast<double>(in.size());
    /* Expect ~0.5; SoundTouch has look-ahead overhead so allow
     * 15% window. */
    CHECK(ratio > 0.40);
    CHECK(ratio < 0.60);
}

TEST_CASE("TempoStretcher: tempo=2.0 preserves pitch (crossings/sec)") {
    constexpr int SR       = 44100;
    constexpr double FREQ  = 1000.0;  /* whole-Hz divisor of SR */
    constexpr double DUR_S = 1.0;

    const auto in   = make_sine(SR, FREQ, DUR_S);
    const auto out  = stretch_mono(in, SR, 2.0);

    /* Input: 1s × 1000 Hz × 2 crossings/cycle = ~2000 crossings.
     * Output after tempo=2: ~0.5s of audio, so ~1000 crossings
     * remain — but crossings-per-second should be the same
     * (2000/s) because pitch didn't change. */
    const int in_cx  = count_zero_crossings(in);
    const int out_cx = count_zero_crossings(out);

    const double in_cx_per_sec = static_cast<double>(in_cx) / DUR_S;
    const double out_cx_per_sec = static_cast<double>(out_cx) /
        (static_cast<double>(out.size()) / SR);

    /* Tolerance: SoundTouch's time-stretch introduces some edge
     * artifacts; allow 5% deviation in crossings/sec. */
    const double rel_err = std::abs(out_cx_per_sec - in_cx_per_sec) /
                            in_cx_per_sec;
    CHECK(rel_err < 0.05);
}

TEST_CASE("TempoStretcher: move ctor / assignment wire through") {
    me::audio::TempoStretcher a(44100, 1);
    a.set_tempo(1.5);
    CHECK(a.sample_rate() == 44100);
    CHECK(a.channels() == 1);

    me::audio::TempoStretcher b = std::move(a);
    CHECK(b.sample_rate() == 44100);
    CHECK(b.channels() == 1);
    /* b is now usable; a is in a valid-but-moved-from state. */
    const std::vector<float> tiny = make_sine(44100, 440.0, 0.1);
    b.put_samples(tiny.data(), tiny.size());
    b.flush();
}

#endif  // ME_HAS_SOUNDTOUCH
