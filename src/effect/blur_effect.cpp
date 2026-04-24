#include "effect/blur_effect.hpp"

#include "shaders/generated/fs_blur_h_mtl.bin.h"
#include "shaders/generated/fs_blur_v_mtl.bin.h"
#include "shaders/generated/vs_fullscreen_mtl.bin.h"

#include <bgfx/bgfx.h>

namespace me::effect {

namespace {

/* Same full-screen triangle as ColorCorrectEffect — duplicated
 * by design in phase-1 (each effect owns its VBO). If a future
 * cycle ships 5+ effects and allocation pressure becomes
 * visible, a shared-geometry helper in the gpu module can
 * collapse them. */
constexpr float kFullscreenTriangleVerts[] = {
    -1.0f, -1.0f,   0.0f, 0.0f,
     3.0f, -1.0f,   2.0f, 0.0f,
    -1.0f,  3.0f,   0.0f, 2.0f,
};

}  // namespace

BlurEffect::BlurEffect(Direction dir, Params params)
    : dir_(dir), params_(params) {

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
    bgfx::ShaderHandle fsh;
    if (dir_ == Direction::Horizontal) {
        fsh = bgfx::createShader(
            bgfx::makeRef(fs_blur_h_mtl, sizeof(fs_blur_h_mtl)));
    } else {
        fsh = bgfx::createShader(
            bgfx::makeRef(fs_blur_v_mtl, sizeof(fs_blur_v_mtl)));
    }

    program_ = bgfx::createProgram(vsh, fsh, /*_destroyShaders=*/true);

    s_src_   = bgfx::createUniform("s_src",        bgfx::UniformType::Sampler);
    u_texel_ = bgfx::createUniform("u_blur_texel", bgfx::UniformType::Vec4);

    valid_ = bgfx::isValid(vbh_) && bgfx::isValid(program_) &&
             bgfx::isValid(s_src_) && bgfx::isValid(u_texel_);
}

BlurEffect::~BlurEffect() {
    if (bgfx::isValid(u_texel_)) bgfx::destroy(u_texel_);
    if (bgfx::isValid(s_src_))   bgfx::destroy(s_src_);
    if (bgfx::isValid(program_)) bgfx::destroy(program_);
    if (bgfx::isValid(vbh_))     bgfx::destroy(vbh_);
}

void BlurEffect::submit(bgfx::ViewId            view_id,
                        bgfx::TextureHandle     src,
                        bgfx::FrameBufferHandle dst) const {
    if (!valid_) return;

    bgfx::setViewFrameBuffer(view_id, dst);
    bgfx::setViewClear(view_id, BGFX_CLEAR_NONE);

    bgfx::setTexture(0, s_src_, src);

    /* texel.xy = (1/W, 1/H). Caller provides W/H via ctor Params.
     * Zero dimensions produce divide-by-zero artifacts — guard by
     * returning early rather than issuing a bad uniform. */
    if (params_.pass_width <= 0 || params_.pass_height <= 0) return;

    const float texel[4] = {
        1.0f / static_cast<float>(params_.pass_width),
        1.0f / static_cast<float>(params_.pass_height),
        0.0f,
        0.0f,
    };
    bgfx::setUniform(u_texel_, texel);

    bgfx::setVertexBuffer(0, vbh_);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);

    bgfx::submit(view_id, program_);
}

}  // namespace me::effect
