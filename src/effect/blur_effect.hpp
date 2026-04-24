/*
 * me::effect::BlurEffect — single-direction box blur pass.
 *
 * Second real GpuEffect. Separable two-pass design: build a chain
 * of { BlurEffect(Horizontal), BlurEffect(Vertical) }, let
 * GpuEffectChain's ping-pong scratch wire the intermediate
 * framebuffer automatically.
 *
 * Phase-1 specifics:
 *   - 3-tap box blur per pass (radius = 1 texel). Wider kernels
 *     or Gaussian weights are future work; separable shape means
 *     they only touch the shader.
 *   - Texel step is passed via a `u_blur_texel` vec4 uniform
 *     (xy = 1/width, 1/height) — computed at construction from
 *     the `pass_width` / `pass_height` Params. Caller is
 *     responsible for matching those to the src texture the pass
 *     samples from.
 *   - Metal-only today (shader bytecode limitation); non-Metal
 *     renderers get `valid() == false` + submit() no-op, matching
 *     ColorCorrectEffect's fallback pattern.
 *
 * Ownership: owns a VBO (full-screen triangle — same geometry as
 * ColorCorrectEffect, intentionally duplicated per effect for
 * phase-1 simplicity), a shader program, and two uniforms
 * (sampler + texel vec4). Ctor + dtor MUST run on the bgfx API
 * thread.
 */
#pragma once

#include "effect/gpu_effect.hpp"

#include <bgfx/bgfx.h>

namespace me::effect {

class BlurEffect final : public GpuEffect {
public:
    enum class Direction {
        Horizontal = 0,
        Vertical   = 1,
    };

    struct Params {
        /* Dimensions of the source texture this pass samples from.
         * Used to compute the texel step uniform. For a symmetric
         * H+V chain both effects typically see the same dimensions
         * (scratch textures match dst size). */
        int pass_width  = 0;
        int pass_height = 0;
    };

    BlurEffect(Direction dir, Params params);
    ~BlurEffect() override;

    BlurEffect(const BlurEffect&)            = delete;
    BlurEffect& operator=(const BlurEffect&) = delete;

    void submit(bgfx::ViewId            view_id,
                bgfx::TextureHandle     src,
                bgfx::FrameBufferHandle dst) const override;

    const char* kind() const noexcept override {
        return dir_ == Direction::Horizontal ? "blur-h" : "blur-v";
    }

    bool valid() const noexcept { return valid_; }

private:
    Direction                dir_;
    Params                   params_;
    bool                     valid_   = false;
    bgfx::VertexBufferHandle vbh_     = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle      program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      s_src_   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_texel_ = BGFX_INVALID_HANDLE;
};

}  // namespace me::effect
