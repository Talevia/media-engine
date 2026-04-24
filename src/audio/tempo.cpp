#include "audio/tempo.hpp"

#include <SoundTouch.h>

namespace me::audio {

TempoStretcher::TempoStretcher(int sample_rate, int channels)
    : sample_rate_(sample_rate),
      channels_(channels),
      st_(std::make_unique<soundtouch::SoundTouch>()) {
    st_->setSampleRate(static_cast<unsigned int>(sample_rate_));
    st_->setChannels(static_cast<unsigned int>(channels_));
}

TempoStretcher::~TempoStretcher() = default;

TempoStretcher::TempoStretcher(TempoStretcher&&) noexcept            = default;
TempoStretcher& TempoStretcher::operator=(TempoStretcher&&) noexcept = default;

void TempoStretcher::set_tempo(double tempo) {
    /* SoundTouch's `setTempo` takes the ratio directly (>1 = faster).
     * It's SoundTouch's authoritative API for "speed without pitch
     * shift" per docs; `setRate` by contrast changes pitch too. */
    st_->setTempo(static_cast<float>(tempo));
}

void TempoStretcher::put_samples(const float* samples, std::size_t n_frames) {
    st_->putSamples(samples, static_cast<unsigned int>(n_frames));
}

std::size_t TempoStretcher::receive_samples(float* out, std::size_t cap_frames) {
    /* receiveSamples returns unsigned int count of frames pulled. */
    const unsigned int got = st_->receiveSamples(
        out, static_cast<unsigned int>(cap_frames));
    return static_cast<std::size_t>(got);
}

void TempoStretcher::flush() {
    st_->flush();
}

}  // namespace me::audio
