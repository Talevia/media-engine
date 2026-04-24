#include "effect/color_correct_effect.hpp"

#include "shaders/generated/fs_color_correct_mtl.bin.h"
#include "shaders/generated/vs_fullscreen_mtl.bin.h"

#include <bgfx/bgfx.h>

namespace me::effect {

namespace {

/* Full-screen-covering single triangle — one vertex past each
 * viewport edge so rasterization covers [-1, +1] × [-1, +1] in
 * clip space with no overdraw. Texcoords scale beyond [0, 1] and
 * get clamped by BGFX_SAMPLER_*_CLAMP on the source texture.
 *
 * Interleaved float2 position + float2 texcoord0, matching
 * varying.def.sc:
 *   a_position  : POSITION   (float2)
 *   a_texcoord0 : TEXCOORD0  (float2)
 */
constexpr float kFullscreenTriangleVerts[] = {
    -1.0f, -1.0f,   0.0f, 0.0f,
     3.0f, -1.0f,   2.0f, 0.0f,
    -1.0f,  3.0f,   0.0f, 2.0f,
};

}  // namespace

ColorCorrectEffect::ColorCorrectEffect()
    : ColorCorrectEffect(Params{}) {}

ColorCorrectEffect::ColorCorrectEffect(Params params)
    : params_(params) {

    /* Today we only ship Metal bytecode. Fallback silently on
     * other renderers — the effect reports valid() == false and
     * submit() becomes a no-op. Extending to Vulkan / D3D12 /
     * OpenGL = compile the same .sc files with the appropriate
     * shaderc --profile flag, commit the new header, add a branch
     * here. */
    if (bgfx::getRendererType() != bgfx::RendererType::Metal) {
        return;
    }

    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

    vbh_ = bgfx::createVertexBuffer(
        bgfx::makeRef(kFullscreenTriangleVerts,
                      sizeof(kFullscreenTriangleVerts)),
        layout);

    bgfx::ShaderHandle vsh = bgfx::createShader(
        bgfx::makeRef(vs_fullscreen_mtl, sizeof(vs_fullscreen_mtl)));
    bgfx::ShaderHandle fsh = bgfx::createShader(
        bgfx::makeRef(fs_color_correct_mtl, sizeof(fs_color_correct_mtl)));

    /* createProgram with `_destroyShaders=true` hands shader
     * ownership to the program — don't manually destroy vsh/fsh. */
    program_ = bgfx::createProgram(vsh, fsh, /*_destroyShaders=*/true);

    s_src_    = bgfx::createUniform("s_src",
                                     bgfx::UniformType::Sampler);
    u_params_ = bgfx::createUniform("u_color_correct_params",
                                     bgfx::UniformType::Vec4);

    valid_ = bgfx::isValid(vbh_) && bgfx::isValid(program_) &&
             bgfx::isValid(s_src_) && bgfx::isValid(u_params_);
}

ColorCorrectEffect::~ColorCorrectEffect() {
    if (bgfx::isValid(u_params_)) bgfx::destroy(u_params_);
    if (bgfx::isValid(s_src_))    bgfx::destroy(s_src_);
    if (bgfx::isValid(program_))  bgfx::destroy(program_);
    if (bgfx::isValid(vbh_))      bgfx::destroy(vbh_);
}

void ColorCorrectEffect::submit(bgfx::ViewId            view_id,
                                bgfx::TextureHandle     src,
                                bgfx::FrameBufferHandle dst) const {
    if (!valid_) return;

    bgfx::setViewFrameBuffer(view_id, dst);
    /* View rect is set by the caller (typically GpuEffectChain::
     * submit). Not setting here avoids double-work + lets single-
     * effect callers with custom rects work. */
    bgfx::setViewClear(view_id, BGFX_CLEAR_NONE);

    bgfx::setTexture(0, s_src_, src);

    const float p4[4] = {
        params_.brightness,
        params_.contrast,
        params_.saturation,
        0.0f,
    };
    bgfx::setUniform(u_params_, p4);

    bgfx::setVertexBuffer(0, vbh_);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);

    bgfx::submit(view_id, program_);
}

}  // namespace me::effect
