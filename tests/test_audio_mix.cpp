/*
 * test_audio_mix — numerical tripwire for me::audio mix/limiter kernels.
 *
 * Pure math coverage matching the bullet's specified anchors:
 *   - `mix_samples`: sum of scaled inputs; empty set silence; identity
 *     (1 input + gain 1.0 = passthrough); gain scaling; determinism.
 *   - `peak_limiter`: below-threshold linear pass-through; overage
 *     compressed to |y| ≤ 1.0; symmetry around zero; peak reporting.
 *   - `db_to_linear`: 0 dB → 1.0; -6 dB ≈ 0.501; -∞ dB → 0.0 exactly.
 *
 * Tolerances chosen for IEEE-754 float32 — most assertions are exact
 * (single-op arithmetic), a few allow ~1e-5 for tanh() and pow10.
 */
#include <doctest/doctest.h>

#include "audio/mix.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using me::audio::db_to_linear;
using me::audio::mix_samples;
using me::audio::peak_limiter;

namespace {

constexpr float F_EPS = 1e-5f;

}  // namespace

TEST_CASE("mix_samples: empty input set writes silence") {
    std::vector<float> out(8, 42.0f);
    mix_samples(nullptr, nullptr, /*num_inputs=*/0, /*num_samples=*/8, out.data());
    for (float v : out) CHECK(v == 0.0f);
}

TEST_CASE("mix_samples: single input at unity gain is a copy") {
    std::vector<float> src(4);
    src = {0.1f, -0.3f, 0.5f, -0.9f};
    std::vector<float> out(4, 42.0f);
    const float* inputs[1] = { src.data() };
    const float  gains[1]  = { 1.0f };
    mix_samples(inputs, gains, 1, 4, out.data());
    for (size_t i = 0; i < 4; ++i) CHECK(out[i] == src[i]);
}

TEST_CASE("mix_samples: two inputs 1.0 + -inf dB (= 0.0) → 1.0 (bullet anchor)") {
    std::vector<float> a(4, 1.0f);
    std::vector<float> b(4, 1.0f);
    const float* inputs[2] = { a.data(), b.data() };
    const float  gains[2]  = { 1.0f, 0.0f };    /* b muted */
    std::vector<float> out(4, -1.0f);
    mix_samples(inputs, gains, 2, 4, out.data());
    for (float v : out) CHECK(v == 1.0f);
}

TEST_CASE("mix_samples: 0.5 + 0.5 inputs with unity gains sum to 1.0") {
    std::vector<float> a(4, 0.5f);
    std::vector<float> b(4, 0.5f);
    const float* inputs[2] = { a.data(), b.data() };
    const float  gains[2]  = { 1.0f, 1.0f };
    std::vector<float> out(4, 0.0f);
    mix_samples(inputs, gains, 2, 4, out.data());
    for (float v : out) CHECK(v == 1.0f);
}

TEST_CASE("mix_samples: per-input gain scales independently") {
    std::vector<float> a(4, 1.0f);
    std::vector<float> b(4, 1.0f);
    const float* inputs[2] = { a.data(), b.data() };
    const float  gains[2]  = { 0.25f, 0.75f };
    std::vector<float> out(4, 0.0f);
    mix_samples(inputs, gains, 2, 4, out.data());
    for (float v : out) CHECK(v == doctest::Approx(1.0f).epsilon(F_EPS));
}

TEST_CASE("mix_samples: determinism — same inputs yield bit-identical output") {
    std::vector<float> a(16), b(16);
    for (size_t i = 0; i < 16; ++i) {
        a[i] = std::sin(float(i) * 0.3f) * 0.7f;
        b[i] = std::cos(float(i) * 0.4f) * 0.4f;
    }
    const float* inputs[2] = { a.data(), b.data() };
    const float  gains[2]  = { 0.6f, 0.9f };
    std::vector<float> o1(16, 0.0f), o2(16, 0.0f);
    mix_samples(inputs, gains, 2, 16, o1.data());
    mix_samples(inputs, gains, 2, 16, o2.data());
    CHECK(o1 == o2);
}

TEST_CASE("peak_limiter: below threshold pass-through is exact") {
    std::vector<float> s = {0.1f, -0.2f, 0.5f, -0.9f, 0.94f};
    const std::vector<float> orig = s;
    const float peak = peak_limiter(s.data(), s.size(), 0.95f);
    for (size_t i = 0; i < s.size(); ++i) CHECK(s[i] == orig[i]);
    CHECK(peak == doctest::Approx(0.94f).epsilon(F_EPS));
}

TEST_CASE("peak_limiter: 1.5 input is compressed to |output| <= 1.0 (bullet anchor)") {
    std::vector<float> s = {1.5f, -1.5f, 2.0f, -3.0f};
    peak_limiter(s.data(), s.size(), 0.95f);
    for (float v : s) {
        CHECK(std::fabs(v) <= 1.0f);
        CHECK(std::fabs(v) > 0.95f);   /* still above threshold after knee */
    }
}

TEST_CASE("peak_limiter: symmetry around zero") {
    std::vector<float> pos = {1.2f, 1.5f, 2.0f};
    std::vector<float> neg = {-1.2f, -1.5f, -2.0f};
    peak_limiter(pos.data(), pos.size(), 0.95f);
    peak_limiter(neg.data(), neg.size(), 0.95f);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(pos[i] == doctest::Approx(-neg[i]).epsilon(F_EPS));
    }
}

TEST_CASE("peak_limiter: returns observed peak before compression") {
    std::vector<float> s = {0.3f, 1.5f, -2.0f, 0.5f};
    const float p = peak_limiter(s.data(), s.size(), 0.95f);
    CHECK(p == doctest::Approx(2.0f).epsilon(F_EPS));
}

TEST_CASE("db_to_linear: 0 dB → 1.0 exactly") {
    CHECK(db_to_linear(0.0f) == 1.0f);
}

TEST_CASE("db_to_linear: -6 dB → ~0.501 (half amplitude)") {
    CHECK(db_to_linear(-6.0f) == doctest::Approx(0.5011872f).epsilon(F_EPS));
}

TEST_CASE("db_to_linear: -20 dB → 0.1 exactly") {
    CHECK(db_to_linear(-20.0f) == doctest::Approx(0.1f).epsilon(F_EPS));
}

TEST_CASE("db_to_linear: -infinity dB → 0.0 exactly (no subnormal)") {
    CHECK(db_to_linear(-std::numeric_limits<float>::infinity()) == 0.0f);
}

TEST_CASE("peak_limiter: defensive clamp on pathological threshold") {
    /* threshold too low / too high gets clamped to [0.5, 0.99]; math
     * still produces finite, bounded output. */
    std::vector<float> s = {1.5f};
    peak_limiter(s.data(), 1, /*threshold=*/0.1f);   /* clamped to 0.5 */
    CHECK(std::fabs(s[0]) <= 1.0f);

    std::vector<float> s2 = {1.5f};
    peak_limiter(s2.data(), 1, /*threshold=*/1.5f);  /* clamped to 0.99 */
    CHECK(std::fabs(s2[0]) <= 1.0f);
}
