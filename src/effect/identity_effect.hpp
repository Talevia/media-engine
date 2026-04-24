/*
 * me::effect::IdentityEffect — no-op Effect.
 *
 * Exists solely to prove end-to-end Effect / EffectChain plumbing
 * (add to chain → chain.apply() calls this → buffer unchanged).
 * Once the first real GPU effect lands (blur / color-correct /
 * LUT) this one stays around as a test-only fixture for chain
 * optimizer behavior (e.g. "identity in the middle of a chain is
 * elided by the GPU merger").
 */
#pragma once

#include "effect/effect.hpp"

namespace me::effect {

class IdentityEffect final : public Effect {
public:
    void apply(std::uint8_t* /*rgba*/,
               int           /*width*/,
               int           /*height*/,
               std::size_t   /*stride_bytes*/) const override {
        /* No-op by design. */
    }
    const char* kind() const noexcept override { return "identity"; }
};

}  // namespace me::effect
