/*
 * me::effect::Effect — abstract base for compositing effects.
 *
 * Scope-A slice of `effect-registry-api-skeleton` (M3 bootstrap).
 * Lays the in-memory abstraction that future GPU effects (blur /
 * color-correct / LUT) and the EffectChain optimizer will build
 * against. No public C ABI yet: the M3 exit criteria describe
 * what effects must do, but the precise ABI shape depends on which
 * effects land first. Internal interface stays moldable until
 * the bgfx backend + first real effect proves the contract.
 *
 * Contract (phase-1 CPU path — will add GPU overrides later):
 *   - apply(rgba, W, H, stride_bytes) reads + writes in-place.
 *   - Must be deterministic: same input → same bytes.
 *   - apply() is pure w.r.t. the Effect instance; no per-frame
 *     state across calls (chain-level state, if needed, lives in
 *     the caller).
 *
 * Phase-1 limits (documented, not enforced in the interface):
 *   - RGBA8 only (matches ComposeSink's working buffer format).
 *   - No multi-buffer effects (e.g. convolution with radius > 1
 *     per pass works by reading adjacent pixels within the same
 *     buffer; effects needing a separate src / dst pass add that
 *     overload in a later cycle).
 *   - Stride == W × 4 assumed by the ComposeSink caller; interface
 *     accepts arbitrary stride for future flexibility.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace me::effect {

/* Abstract effect. One instance per configured effect in a
 * composition — callers own the pointer lifetime. */
class Effect {
public:
    virtual ~Effect() = default;

    /* Apply this effect to an RGBA8 buffer in-place. */
    virtual void apply(std::uint8_t* rgba,
                       int           width,
                       int           height,
                       std::size_t   stride_bytes) const = 0;

    /* Debug-visible kind tag. Used by logs + tests to verify the
     * right effect landed in a chain. Not a stable identifier for
     * persistence — internal only. */
    virtual const char* kind() const noexcept = 0;
};

}  // namespace me::effect
