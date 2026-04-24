/*
 * me::effect::EffectChain — ordered sequence of Effects applied
 * in-place to a buffer.
 *
 * Scope-A slice of `effect-registry-api-skeleton` (M3 bootstrap).
 * Future M3 exit criterion "EffectChain 能把连续 ≥ 2 个像素级
 * effect 合并成单 pass" will turn the CPU-iterate path below into
 * a GPU shader-merge path when the bgfx backend + first real
 * GPU effects land. For now the chain is the simplest thing:
 * linear invocation of each effect's apply() in declaration order.
 *
 * Ownership:
 *   - Chain owns its Effects via unique_ptr.
 *   - Adding a nullptr is a caller bug (asserted in debug builds;
 *     silently skipped in release).
 *   - Chain is move-only, non-copyable — matches ComposeSink's
 *     expected ownership (one chain per clip / composition).
 */
#pragma once

#include "effect/effect.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace me::effect {

class EffectChain {
public:
    EffectChain() = default;
    ~EffectChain() = default;

    EffectChain(const EffectChain&)            = delete;
    EffectChain& operator=(const EffectChain&) = delete;
    EffectChain(EffectChain&&) noexcept            = default;
    EffectChain& operator=(EffectChain&&) noexcept = default;

    /* Append an Effect to the end of the chain. Chain takes
     * ownership. No-op if `e` is nullptr. */
    void append(std::unique_ptr<Effect> e) {
        assert(e && "EffectChain::append: null Effect pointer");
        if (!e) return;
        effects_.push_back(std::move(e));
    }

    /* Apply all Effects in declaration order, each reading + writing
     * the same rgba buffer. Caller owns the buffer. */
    void apply(std::uint8_t* rgba,
               int           width,
               int           height,
               std::size_t   stride_bytes) const {
        for (const auto& e : effects_) {
            e->apply(rgba, width, height, stride_bytes);
        }
    }

    std::size_t size() const noexcept { return effects_.size(); }
    bool        empty() const noexcept { return effects_.empty(); }

    /* Peek at the Nth effect's kind tag (for test introspection /
     * future chain-optimizer debug output). Out-of-range returns
     * nullptr. */
    const char* kind_at(std::size_t i) const noexcept {
        if (i >= effects_.size()) return nullptr;
        return effects_[i]->kind();
    }

private:
    std::vector<std::unique_ptr<Effect>> effects_;
};

}  // namespace me::effect
