/*
 * me::audio::mix impl. See mix.hpp for contract.
 */
#include "audio/mix.hpp"

#include <cmath>
#include <cstring>

namespace me::audio {

void mix_samples(const float* const* inputs,
                 const float*        gain_linear,
                 std::size_t         num_inputs,
                 std::size_t         num_samples,
                 float*              output) {
    /* num_inputs == 0 (or null inputs): emit silence. Keeps the
     * "passthrough of empty track set" case simple for callers. */
    if (num_inputs == 0 || inputs == nullptr) {
        std::memset(output, 0, num_samples * sizeof(float));
        return;
    }

    for (std::size_t j = 0; j < num_samples; ++j) {
        float sum = 0.0f;
        for (std::size_t i = 0; i < num_inputs; ++i) {
            sum += inputs[i][j] * gain_linear[i];
        }
        output[j] = sum;
    }
}

float peak_limiter(float*      samples,
                   std::size_t num_samples,
                   float       threshold) {
    /* Defensive clamp on threshold. Keeps the math well-defined even
     * if caller passes out-of-range. */
    if (threshold < 0.5f)  threshold = 0.5f;
    if (threshold > 0.99f) threshold = 0.99f;

    const float headroom = 1.0f - threshold;   /* > 0 */
    float peak = 0.0f;

    for (std::size_t j = 0; j < num_samples; ++j) {
        const float x    = samples[j];
        const float absx = std::fabs(x);
        if (absx > peak) peak = absx;

        if (absx <= threshold) {
            /* Linear region — unchanged. */
            continue;
        }

        /* Soft-knee: map the overage through tanh so that
         * x = threshold → y = threshold (continuous at the knee)
         * x → ∞         → y → 1.0 (saturating). The tanh scaling
         * uses `headroom` so the soft region extends smoothly from
         * threshold up to ±1.0. */
        const float over     = absx - threshold;
        const float compressed = std::tanh(over / headroom) * headroom;
        const float y_abs    = threshold + compressed;
        samples[j] = (x >= 0.0f) ? y_abs : -y_abs;
    }

    return peak;
}

float db_to_linear(float db) {
    /* -∞ dB → 0 exactly. Any "large negative" dB already produces
     * an exponentially tiny linear value, but making -∞ exact lets
     * callers compose mute tracks without subnormal arithmetic. */
    if (std::isinf(db) && db < 0.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

}  // namespace me::audio
