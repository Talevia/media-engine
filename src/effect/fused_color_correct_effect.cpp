#include "effect/fused_color_correct_effect.hpp"

#include "shaders/generated/fs_color_correct_double_mtl.bin.h"
#include "shaders/generated/vs_fullscreen_mtl.bin.h"

#include <bgfx/bgfx.h>

namespace me::effect {

namespace {

constexpr float kFullscreenTriangleVerts[] = {
    -1.0f, -1.0f,   0.0f, 0.0f,
     3.0f, -1.0f,   2.0f, 0.0f,
    -1.0f,  3.0f,   0.0f, 2.0f,
};

}  // namespace

FusedColorCorrectEffect::FusedColorCorrectEffect(
    ColorCorrectEffect::Params stage_a,
    ColorCorrectEffect::Params stage_b)
    : a_(stage_a), b_(stage_b) {

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
        bgfx::makeRef(fs_color_correct_double_mtl,
                      sizeof(fs_color_correct_double_mtl)));
    program_ = bgfx::createProgram(vsh, fsh, /*_destroyShaders=*/true);

    s_src_ = bgfx::createUniform("s_src",          bgfx::UniformType::Sampler);
    u_a_   = bgfx::createUniform("u_cc_params_a",  bgfx::UniformType::Vec4);
    u_b_   = bgfx::createUniform("u_cc_params_b",  bgfx::UniformType::Vec4);

    valid_ = bgfx::isValid(vbh_) && bgfx::isValid(program_) &&
             bgfx::isValid(s_src_) && bgfx::isValid(u_a_) &&
             bgfx::isValid(u_b_);
}

FusedColorCorrectEffect::~FusedColorCorrectEffect() {
    if (bgfx::isValid(u_b_))     bgfx::destroy(u_b_);
    if (bgfx::isValid(u_a_))     bgfx::destroy(u_a_);
    if (bgfx::isValid(s_src_))   bgfx::destroy(s_src_);
    if (bgfx::isValid(program_)) bgfx::destroy(program_);
    if (bgfx::isValid(vbh_))     bgfx::destroy(vbh_);
}

void FusedColorCorrectEffect::submit(
    bgfx::ViewId            view_id,
    bgfx::TextureHandle     src,
    bgfx::FrameBufferHandle dst) const {
    if (!valid_) return;

    bgfx::setViewFrameBuffer(view_id, dst);
    bgfx::setViewClear(view_id, BGFX_CLEAR_NONE);

    bgfx::setTexture(0, s_src_, src);

    const float pa[4] = { a_.brightness, a_.contrast, a_.saturation, 0.0f };
    const float pb[4] = { b_.brightness, b_.contrast, b_.saturation, 0.0f };
    bgfx::setUniform(u_a_, pa);
    bgfx::setUniform(u_b_, pb);

    bgfx::setVertexBuffer(0, vbh_);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);

    bgfx::submit(view_id, program_);
}

}  // namespace me::effect
