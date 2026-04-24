/*
 * me::effect::FusedColorCorrectEffect — two chained
 * ColorCorrectEffects fused into a single fragment-shader pass.
 *
 * Produced by `GpuEffectChain::compile()` when two adjacent
 * ColorCorrectEffect instances are detected in a chain. The
 * fused shader (fs_color_correct_double.sc) applies stage A's
 * brightness / contrast / saturation, then stage B's, in one
 * draw call — eliminating the intermediate framebuffer
 * bandwidth the two-pass chain would have consumed.
 *
 * M3 exit criterion "EffectChain 能把连续 ≥ 2 个像素级 effect
 * 合并成单 pass" is closed by this + compile(); extending the
 * fused catalog to other compatible pairs (CC + LUT, LUT + LUT,
 * etc.) is mechanical follow-up when visual workloads demand.
 *
 * Metal-only today (shader bytecode limitation). Non-Metal gets
 * `valid() == false`; callers should not invoke compile() on
 * chains they expect to run on non-Metal renderers, or should
 * tolerate the original two-pass chain staying intact.
 */
#pragma once

#include "effect/color_correct_effect.hpp"
#include "effect/gpu_effect.hpp"

#include <bgfx/bgfx.h>

namespace me::effect {

class FusedColorCorrectEffect final : public GpuEffect {
public:
    FusedColorCorrectEffect(ColorCorrectEffect::Params stage_a,
                            ColorCorrectEffect::Params stage_b);
    ~FusedColorCorrectEffect() override;

    FusedColorCorrectEffect(const FusedColorCorrectEffect&)            = delete;
    FusedColorCorrectEffect& operator=(const FusedColorCorrectEffect&) = delete;

    void submit(bgfx::ViewId            view_id,
                bgfx::TextureHandle     src,
                bgfx::FrameBufferHandle dst) const override;

    const char* kind() const noexcept override {
        return "fused-color-correct";
    }

    bool valid() const noexcept { return valid_; }

    ColorCorrectEffect::Params stage_a() const noexcept { return a_; }
    ColorCorrectEffect::Params stage_b() const noexcept { return b_; }

private:
    ColorCorrectEffect::Params a_;
    ColorCorrectEffect::Params b_;
    bool                       valid_   = false;
    bgfx::VertexBufferHandle   vbh_     = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle        program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle        s_src_   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle        u_a_     = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle        u_b_     = BGFX_INVALID_HANDLE;
};

}  // namespace me::effect
