#include "effect/gpu_effect_chain.hpp"

#include "effect/color_correct_effect.hpp"
#include "effect/fused_color_correct_effect.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace me::effect {

void GpuEffectChain::compile() {
    /* Left-to-right greedy fusion. For each position i: if
     * effects_[i] and effects_[i+1] are both ColorCorrectEffect,
     * replace the pair with a single FusedColorCorrectEffect.
     * Otherwise, carry effects_[i] through unchanged. Time is
     * O(n) since we consume one or two positions per iteration.
     *
     * dynamic_cast requires RTTI + polymorphic base. GpuEffect has
     * a virtual destructor, so this is well-defined. RTTI is on
     * for this build (no -fno-rtti in CMakeLists.txt). */
    std::vector<std::unique_ptr<GpuEffect>> out;
    out.reserve(effects_.size());

    for (std::size_t i = 0; i < effects_.size(); ) {
        if (i + 1 < effects_.size()) {
            auto* a = dynamic_cast<ColorCorrectEffect*>(effects_[i].get());
            auto* b = dynamic_cast<ColorCorrectEffect*>(effects_[i + 1].get());
            if (a != nullptr && b != nullptr) {
                out.push_back(std::make_unique<FusedColorCorrectEffect>(
                    a->params(), b->params()));
                i += 2;
                continue;
            }
        }
        out.push_back(std::move(effects_[i]));
        ++i;
    }

    effects_ = std::move(out);
}

}  // namespace me::effect
