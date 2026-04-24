/*
 * me::effect::ColorCorrectEffect — brightness / contrast / saturation
 * GPU effect.
 *
 * First real GpuEffect implementation — closes M3's "≥ 3 GPU effect
 * (blur / color-correct / LUT)" exit criterion partway (1/3). Uses
 * a full-screen-triangle vertex shader + color-correct fragment
 * shader. Shaders are precompiled via bgfx's shaderc tool (from
 * src/effect/shaders/fs_color_correct.sc + vs_fullscreen.sc +
 * varying.def.sc) into Metal bytecode headers and embedded at
 * compile time. The generator command is documented in
 * src/effect/shaders/README — rerun shaderc + commit the updated
 * headers when shaders change.
 *
 * Platform coverage today: Metal only. Non-Metal renderers
 * (Vulkan / D3D12 / OpenGL) get a fallback valid_ == false, and
 * submit() becomes a no-op — the consumer chain sees the src
 * texture passed through unmodified. Extending to other backends
 * is mechanical (add shaderc invocations for --profile spirv /
 * s_6_0 / 430 → more .bin.h headers → ctor selects by renderer);
 * deferred to when those platforms need to render.
 *
 * Ownership: each instance owns its own vertex buffer, shader
 * program, and two uniform handles (sampler + params vec4). Ctor
 * creates all bgfx handles — MUST be called on the bgfx API
 * thread (via BgfxGpuBackend::submit_on_render_thread). Dtor
 * destroys handles; also API-thread-only.
 */
#pragma once

#include "effect/gpu_effect.hpp"

#include <bgfx/bgfx.h>

namespace me::effect {

class ColorCorrectEffect final : public GpuEffect {
public:
    struct Params {
        /* Additive luminance offset; typ. [-1, +1]. 0 = identity. */
        float brightness = 0.0f;
        /* Multiplicative contrast around 0.5 pivot; typ. [0, 2]. 1 = identity. */
        float contrast   = 1.0f;
        /* Mix factor against Rec-709 luma; typ. [0, 2]. 1 = identity. */
        float saturation = 1.0f;
    };

    ColorCorrectEffect();
    explicit ColorCorrectEffect(Params params);
    ~ColorCorrectEffect() override;

    ColorCorrectEffect(const ColorCorrectEffect&)            = delete;
    ColorCorrectEffect& operator=(const ColorCorrectEffect&) = delete;

    void set_params(Params p) noexcept { params_ = p; }

    void submit(bgfx::ViewId            view_id,
                bgfx::TextureHandle     src,
                bgfx::FrameBufferHandle dst) const override;

    const char* kind() const noexcept override { return "color-correct"; }

    /* True iff ctor successfully created all bgfx handles for the
     * current renderer. Checks: shader compilation bytecode
     * available for bgfx::getRendererType() AND every handle
     * bgfx::isValid. A false result means submit() is a no-op; the
     * caller should detect this (e.g. via a separate backend caps
     * check) if it matters. */
    bool valid() const noexcept { return valid_; }

private:
    Params                   params_;
    bool                     valid_    = false;
    bgfx::VertexBufferHandle vbh_      = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle      program_  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      s_src_    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      u_params_ = BGFX_INVALID_HANDLE;
};

}  // namespace me::effect
