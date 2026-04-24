/*
 * me::audio::AudioEffectChain — ordered chain of AudioEffects
 * applied in-place to an interleaved PCM buffer.
 *
 * Parallel to me::effect::EffectChain (the CPU video effect
 * chain). Applies each effect's process() in declaration order;
 * a Gain followed by a Pan behaves as "gain-then-pan" rather
 * than the other way around.
 *
 * Move-only. One chain per audio source / clip. No fusion
 * step in phase-1 — audio effects are cheap enough (one or two
 * mults per sample typical) that fusing them yields marginal
 * savings vs the video pass-merge analogue which avoids real
 * framebuffer ping-pong. If a future perf hot spot argues
 * otherwise, fusion lands behind a `compile()` method mirroring
 * the video side.
 */
#pragma once

#include "audio/audio_effect.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace me::audio {

class AudioEffectChain {
public:
    AudioEffectChain() = default;
    ~AudioEffectChain() = default;

    AudioEffectChain(const AudioEffectChain&)            = delete;
    AudioEffectChain& operator=(const AudioEffectChain&) = delete;
    AudioEffectChain(AudioEffectChain&&) noexcept            = default;
    AudioEffectChain& operator=(AudioEffectChain&&) noexcept = default;

    /* Append an effect to the end of the chain. Chain takes
     * ownership; nullptr is a caller bug (asserted in debug,
     * silently dropped in release). */
    void append(std::unique_ptr<AudioEffect> e) {
        assert(e && "AudioEffectChain::append: null AudioEffect");
        if (!e) return;
        effects_.push_back(std::move(e));
    }

    /* Apply all effects in declaration order. */
    void process(float*       samples,
                 std::size_t  n_frames,
                 int          n_channels,
                 int          sample_rate) {
        for (auto& e : effects_) {
            e->process(samples, n_frames, n_channels, sample_rate);
        }
    }

    std::size_t size() const noexcept { return effects_.size(); }
    bool        empty() const noexcept { return effects_.empty(); }

    const char* kind_at(std::size_t i) const noexcept {
        if (i >= effects_.size()) return nullptr;
        return effects_[i]->kind();
    }

private:
    std::vector<std::unique_ptr<AudioEffect>> effects_;
};

}  // namespace me::audio
