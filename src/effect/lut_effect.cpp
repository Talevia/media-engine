#include "effect/lut_effect.hpp"

#include "shaders/generated/fs_lut_mtl.bin.h"
#include "shaders/generated/vs_fullscreen_mtl.bin.h"

#include <bgfx/bgfx.h>

#include <cstddef>
#include <utility>

namespace me::effect {

namespace {

/* Same full-screen triangle as ColorCorrectEffect / BlurEffect. */
constexpr float kFullscreenTriangleVerts[] = {
    -1.0f, -1.0f,   0.0f, 0.0f,
     3.0f, -1.0f,   2.0f, 0.0f,
    -1.0f,  3.0f,   0.0f, 2.0f,
};

/* Convert float-RGB LUT data to RGBA8 for upload. Metal's
 * TextureFormat::RGB32F works but is 12 bytes/texel; RGBA8 is
 * 4 bytes and sufficient for 8-bit color grading. Alpha is
 * stuffed with 0xFF (unused by shader).
 *
 * Range clamp: LUT entries outside [0, 1] are clamped before
 * quantization. Most .cube files are in [0, 1]; HDR-range LUTs
 * would need a float texture — swap format here if that ever
 * lands. */
std::vector<uint8_t> lut_float_to_rgba8(
    const std::vector<float>& rgb_data, std::size_t cells) {
    std::vector<uint8_t> out(cells * 4);
    for (std::size_t i = 0; i < cells; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = rgb_data[i * 3 + c];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            out[i * 4 + c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
        out[i * 4 + 3] = 0xFF;
    }
    return out;
}

}  // namespace

LutEffect::LutEffect(int cube_size, std::vector<float> rgb_float_data)
    : cube_size_(cube_size),
      lut_data_(std::move(rgb_float_data)) {

    if (bgfx::getRendererType() != bgfx::RendererType::Metal) {
        return;
    }
    if (cube_size_ < 2) return;

    const std::size_t cells   = static_cast<std::size_t>(cube_size_) *
                                 cube_size_ * cube_size_;
    const std::size_t expected = cells * 3;
    if (lut_data_.size() != expected) {
        return;
    }

    const auto caps = bgfx::getCaps();
    if (0 == (caps->supported & BGFX_CAPS_TEXTURE_3D)) {
        return;
    }

    /* 3D texture of cube_size³ RGBA8 cells. Trilinear filter via
     * default sampler flags (BGFX_SAMPLER_NONE ≈ linear) + clamp
     * on all three axes so out-of-[0,1] RGB samples grab the
     * boundary cell rather than wrapping.
     *
     * Feed bgfx a COPY of the RGBA8 bytes (not makeRef) because
     * lut_float_to_rgba8 returns a local vector — `copy` is the
     * safe default when the source goes out of scope. */
    auto rgba8 = lut_float_to_rgba8(lut_data_, cells);
    const bgfx::Memory* mem = bgfx::copy(rgba8.data(),
                                          static_cast<uint32_t>(rgba8.size()));

    lut_tex_ = bgfx::createTexture3D(
        static_cast<uint16_t>(cube_size_),
        static_cast<uint16_t>(cube_size_),
        static_cast<uint16_t>(cube_size_),
        /*hasMips=*/false,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
        mem);
    if (!bgfx::isValid(lut_tex_)) return;

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
        bgfx::makeRef(fs_lut_mtl, sizeof(fs_lut_mtl)));
    program_ = bgfx::createProgram(vsh, fsh, /*_destroyShaders=*/true);

    s_src_ = bgfx::createUniform("s_src", bgfx::UniformType::Sampler);
    s_lut_ = bgfx::createUniform("s_lut", bgfx::UniformType::Sampler);

    valid_ = bgfx::isValid(vbh_) && bgfx::isValid(program_) &&
             bgfx::isValid(s_src_) && bgfx::isValid(s_lut_);
}

LutEffect::~LutEffect() {
    if (bgfx::isValid(s_lut_))   bgfx::destroy(s_lut_);
    if (bgfx::isValid(s_src_))   bgfx::destroy(s_src_);
    if (bgfx::isValid(program_)) bgfx::destroy(program_);
    if (bgfx::isValid(vbh_))     bgfx::destroy(vbh_);
    if (bgfx::isValid(lut_tex_)) bgfx::destroy(lut_tex_);
}

void LutEffect::submit(bgfx::ViewId            view_id,
                       bgfx::TextureHandle     src,
                       bgfx::FrameBufferHandle dst) const {
    if (!valid_) return;

    bgfx::setViewFrameBuffer(view_id, dst);
    bgfx::setViewClear(view_id, BGFX_CLEAR_NONE);

    bgfx::setTexture(0, s_src_, src);
    bgfx::setTexture(1, s_lut_, lut_tex_);

    bgfx::setVertexBuffer(0, vbh_);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);

    bgfx::submit(view_id, program_);
}

}  // namespace me::effect
