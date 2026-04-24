/*
 * test_audio_effect_chain — unit coverage for
 * me::audio::AudioEffect + AudioEffectChain + the phase-1
 * concrete effects (Gain / Pan / Lowpass).
 *
 * Pure C++; no bgfx / SoundTouch / libav. Runs in every build.
 */
#include <doctest/doctest.h>

#include "audio/audio_effect_chain.hpp"
#include "audio/gain_audio_effect.hpp"
#include "audio/lowpass_audio_effect.hpp"
#include "audio/pan_audio_effect.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

/* Interleaved sine at `freq_hz`, `duration_seconds`, n channels
 * (all channels emit the same signal). */
std::vector<float> make_sine_interleaved(int sample_rate, double freq_hz,
                                          double duration_seconds,
                                          int n_channels) {
    const std::size_t n_frames = static_cast<std::size_t>(
        static_cast<double>(sample_rate) * duration_seconds);
    std::vector<float> out(n_frames * n_channels);
    for (std::size_t i = 0; i < n_frames; ++i) {
        const double t   = static_cast<double>(i) / sample_rate;
        const float  s   = static_cast<float>(std::sin(2.0 * kPi * freq_hz * t));
        for (int c = 0; c < n_channels; ++c) {
            out[i * n_channels + c] = s;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("GainAudioEffect: identity gain (1.0) preserves samples") {
    me::audio::GainAudioEffect eff(1.0f);
    std::vector<float> samples = { 0.5f, -0.25f, 0.1f, -0.1f };
    const std::vector<float> copy = samples;
    eff.process(samples.data(), /*n_frames=*/2, /*n_channels=*/2, /*sr=*/44100);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CHECK(samples[i] == copy[i]);
    }
}

TEST_CASE("GainAudioEffect: gain=2.0 doubles amplitude") {
    me::audio::GainAudioEffect eff(2.0f);
    std::vector<float> samples = { 0.3f, -0.4f };
    eff.process(samples.data(), 1, 2, 44100);
    CHECK(samples[0] == doctest::Approx(0.6f));
    CHECK(samples[1] == doctest::Approx(-0.8f));
}

TEST_CASE("GainAudioEffect: gain=0 silences") {
    me::audio::GainAudioEffect eff(0.0f);
    std::vector<float> samples = { 0.5f, 0.5f, 0.5f, 0.5f };
    eff.process(samples.data(), 2, 2, 44100);
    for (float s : samples) CHECK(s == doctest::Approx(0.0f));
}

TEST_CASE("PanAudioEffect: pan=0 preserves stereo balance") {
    me::audio::PanAudioEffect eff(0.0f);
    std::vector<float> samples = { 0.5f, 0.5f, 0.3f, 0.3f };
    eff.process(samples.data(), 2, 2, 44100);
    CHECK(samples[0] == doctest::Approx(0.5f));
    CHECK(samples[1] == doctest::Approx(0.5f));
    CHECK(samples[2] == doctest::Approx(0.3f));
    CHECK(samples[3] == doctest::Approx(0.3f));
}

TEST_CASE("PanAudioEffect: pan=+1 zeroes left, preserves right") {
    me::audio::PanAudioEffect eff(1.0f);
    std::vector<float> samples = { 1.0f, 1.0f, 0.5f, 0.5f };
    eff.process(samples.data(), 2, 2, 44100);
    CHECK(samples[0] == doctest::Approx(0.0f));
    CHECK(samples[1] == doctest::Approx(1.0f));
    CHECK(samples[2] == doctest::Approx(0.0f));
    CHECK(samples[3] == doctest::Approx(0.5f));
}

TEST_CASE("PanAudioEffect: pan=-1 zeroes right, preserves left") {
    me::audio::PanAudioEffect eff(-1.0f);
    std::vector<float> samples = { 0.8f, 0.8f };
    eff.process(samples.data(), 1, 2, 44100);
    CHECK(samples[0] == doctest::Approx(0.8f));
    CHECK(samples[1] == doctest::Approx(0.0f));
}

TEST_CASE("PanAudioEffect: non-stereo input is a no-op") {
    me::audio::PanAudioEffect eff(1.0f);
    std::vector<float> samples = { 0.5f, 0.5f, 0.5f };  // mono, 3 frames
    const std::vector<float> copy = samples;
    eff.process(samples.data(), 3, 1, 44100);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CHECK(samples[i] == copy[i]);
    }
}

TEST_CASE("LowpassAudioEffect: DC passes through unchanged") {
    /* DC (constant signal) is by definition a 0 Hz component;
     * any low-pass filter with a positive cutoff passes it.
     * With y[0] = a * x[0] + (1-a) * 0 = a * x[0] (initial
     * state = 0), the first sample is attenuated; steady state
     * reaches x exactly. Check the steady-state tail. */
    me::audio::LowpassAudioEffect eff(1000.0f);
    std::vector<float> samples(1000, 1.0f);
    eff.process(samples.data(), samples.size(), 1, 44100);
    /* After a thousand samples at 44.1 kHz with 1 kHz cutoff,
     * the filter is in steady state for DC. */
    CHECK(samples.back() == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("LowpassAudioEffect: high frequency is attenuated") {
    /* Feed a sine at 20 kHz (near Nyquist) through a 500 Hz
     * cutoff. Output RMS should be dramatically less than
     * input RMS. */
    const auto input = make_sine_interleaved(44100, 20000.0, 0.1, 1);
    std::vector<float> samples = input;

    me::audio::LowpassAudioEffect eff(500.0f);
    eff.process(samples.data(), samples.size(), 1, 44100);

    auto rms = [](const std::vector<float>& v) {
        double sum_sq = 0.0;
        for (float s : v) sum_sq += static_cast<double>(s) * s;
        return std::sqrt(sum_sq / v.size());
    };
    const double input_rms  = rms(input);
    const double output_rms = rms(samples);

    /* Filter attenuation at 20 kHz with 500 Hz cutoff should be
     * strong — expect < 10% of input amplitude. */
    CHECK(output_rms < input_rms * 0.1);
}

TEST_CASE("LowpassAudioEffect: reset clears per-channel state") {
    me::audio::LowpassAudioEffect eff(1000.0f);
    std::vector<float> samples1(100, 1.0f);
    eff.process(samples1.data(), samples1.size(), 1, 44100);
    /* After 1s of DC, y_prev ≈ 1.0. */
    eff.reset();
    /* Now feed 0.0 — without reset this would decay from 1.0;
     * after reset it starts from 0. */
    std::vector<float> samples2(10, 0.0f);
    eff.process(samples2.data(), samples2.size(), 1, 44100);
    /* samples2[0] = a * 0 + (1-a) * 0 = 0. */
    CHECK(samples2[0] == doctest::Approx(0.0f));
}

TEST_CASE("AudioEffectChain: empty chain is a no-op") {
    me::audio::AudioEffectChain chain;
    CHECK(chain.empty());
    std::vector<float> samples = { 0.5f, -0.5f };
    const std::vector<float> copy = samples;
    chain.process(samples.data(), 1, 2, 44100);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CHECK(samples[i] == copy[i]);
    }
}

TEST_CASE("AudioEffectChain: gain-then-pan applies in order") {
    me::audio::AudioEffectChain chain;
    chain.append(std::make_unique<me::audio::GainAudioEffect>(2.0f));
    chain.append(std::make_unique<me::audio::PanAudioEffect>(1.0f));
    REQUIRE(chain.size() == 2);
    CHECK(std::string{chain.kind_at(0)} == "gain");
    CHECK(std::string{chain.kind_at(1)} == "pan");

    /* Input: L=R=0.3. After gain=2: L=R=0.6. After pan=+1:
     * L=0, R=0.6. */
    std::vector<float> samples = { 0.3f, 0.3f };
    chain.process(samples.data(), 1, 2, 44100);
    CHECK(samples[0] == doctest::Approx(0.0f));
    CHECK(samples[1] == doctest::Approx(0.6f));
}

TEST_CASE("AudioEffectChain: kind_at out-of-range returns nullptr") {
    me::audio::AudioEffectChain chain;
    chain.append(std::make_unique<me::audio::GainAudioEffect>());
    CHECK(chain.kind_at(0) != nullptr);
    CHECK(chain.kind_at(1) == nullptr);
    CHECK(chain.kind_at(999) == nullptr);
}

TEST_CASE("AudioEffectChain: move ctor transfers effects") {
    me::audio::AudioEffectChain a;
    a.append(std::make_unique<me::audio::GainAudioEffect>(3.0f));
    a.append(std::make_unique<me::audio::GainAudioEffect>(0.5f));

    me::audio::AudioEffectChain b = std::move(a);
    CHECK(b.size() == 2);
    /* Chained gain: 3 * 0.5 = 1.5. */
    std::vector<float> samples = { 1.0f };
    b.process(samples.data(), 1, 1, 44100);
    CHECK(samples[0] == doctest::Approx(1.5f));
}
