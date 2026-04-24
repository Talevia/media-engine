/*
 * test_lut_effect — pixel-level proof that `me::effect::LutEffect`
 * builds a 3D LUT texture and the fragment shader samples it
 * trilinearly.
 *
 * ME_WITH_GPU-only; skips on non-Metal (shader bytecode
 * limitation) and on drivers without BGFX_CAPS_TEXTURE_3D.
 *
 * Coverage:
 *   - Identity 2×2×2 LUT preserves red input: demonstrates the
 *     full pipeline (parse → upload → sample → write).
 *   - Invert LUT (output = 1 - input) on red input produces cyan:
 *     demonstrates the LUT actually remaps color rather than
 *     returning input by accident.
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "effect/lut_effect.hpp"
#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace {

constexpr uint16_t W = 4;
constexpr uint16_t H = 4;

struct Textures {
    bgfx::TextureHandle     src = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     rt  = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     rb  = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle fb  = BGFX_INVALID_HANDLE;
    ~Textures() {
        if (bgfx::isValid(fb))  bgfx::destroy(fb);
        if (bgfx::isValid(rt))  bgfx::destroy(rt);
        if (bgfx::isValid(rb))  bgfx::destroy(rb);
        if (bgfx::isValid(src)) bgfx::destroy(src);
    }
};

void pump_until(uint32_t target) {
    for (int i = 0; i < 10; ++i) {
        if (bgfx::frame() >= target) return;
    }
}

std::vector<uint8_t> solid_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> buf(static_cast<std::size_t>(W) * H * 4);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = a;
    }
    return buf;
}

/* 2x2x2 identity LUT: each corner is the corner's own RGB. With
 * trilinear filtering, interior colors lerp between corners —
 * which is the continuous identity mapping. */
std::vector<float> identity_2x2x2() {
    return {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        1, 1, 0,
        0, 0, 1,
        1, 0, 1,
        0, 1, 1,
        1, 1, 1,
    };
}

/* 2x2x2 inversion LUT: each corner maps to (1 - corner). Pure
 * red input (1, 0, 0) should produce cyan (0, 1, 1). */
std::vector<float> inversion_2x2x2() {
    return {
        1, 1, 1,
        0, 1, 1,
        1, 0, 1,
        0, 0, 1,
        1, 1, 0,
        0, 1, 0,
        1, 0, 0,
        0, 0, 0,
    };
}

bool skip_non_metal() {
    if (bgfx::getRendererType() != bgfx::RendererType::Metal) {
        MESSAGE("Non-Metal renderer — LUT shader is Metal-only; skipping.");
        return true;
    }
    const auto caps = bgfx::getCaps();
    if (0 == (caps->supported & BGFX_CAPS_TEXTURE_3D)) {
        MESSAGE("BGFX_CAPS_TEXTURE_3D missing — skipping.");
        return true;
    }
    return false;
}

std::vector<uint8_t> run_lut(const me::effect::LutEffect& effect,
                              const std::vector<uint8_t>& src_rgba) {
    Textures t;
    const bgfx::Memory* mem = bgfx::makeRef(src_rgba.data(), src_rgba.size());
    t.src = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        mem);
    t.rt = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
    t.rb = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
    t.fb = bgfx::createFrameBuffer(1, &t.rt, false);

    bgfx::setViewRect(0, 0, 0, W, H);
    effect.submit(0, t.src, t.fb);
    bgfx::blit(1, t.rb, 0, 0, t.rt, 0, 0, W, H);

    std::vector<uint8_t> pixels(static_cast<std::size_t>(W) * H * 4, 0);
    const uint32_t ready = bgfx::readTexture(t.rb, pixels.data());
    pump_until(ready);
    return pixels;
}

}  // namespace

TEST_CASE("LutEffect: identity 2x2x2 LUT preserves red input") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::LutEffect effect(2, identity_2x2x2());
        REQUIRE(effect.valid());
        REQUIRE(effect.cube_size() == 2);

        auto pixels = run_lut(effect, solid_rgba(0xFF, 0x00, 0x00, 0xFF));

        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        /* Identity LUT with trilinear filter on RGBA8: pure red
         * (1, 0, 0) stored as (0xFF, 0, 0); one filter step can
         * introduce ±1 ULP. Allow a small tolerance. */
        auto roughly = [](int v, int target) {
            return v >= target - 3 && v <= target + 3;
        };
        CHECK(roughly(pixels[mid + 0], 0xFF));
        CHECK(roughly(pixels[mid + 1], 0x00));
        CHECK(roughly(pixels[mid + 2], 0x00));
        CHECK(pixels[mid + 3] == 0xFF);
    });
}

TEST_CASE("LutEffect: inversion LUT flips red to cyan") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::LutEffect effect(2, inversion_2x2x2());
        REQUIRE(effect.valid());

        auto pixels = run_lut(effect, solid_rgba(0xFF, 0x00, 0x00, 0xFF));

        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        /* red (1, 0, 0) → cyan (0, 1, 1). */
        auto roughly = [](int v, int target) {
            return v >= target - 3 && v <= target + 3;
        };
        CHECK(roughly(pixels[mid + 0], 0x00));
        CHECK(roughly(pixels[mid + 1], 0xFF));
        CHECK(roughly(pixels[mid + 2], 0xFF));
        CHECK(pixels[mid + 3] == 0xFF);
    });
}

TEST_CASE("LutEffect: invalid LUT data results in valid()==false") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    backend->submit_on_render_thread([&] {
        /* cube_size=2 expects 24 floats; give 3. */
        me::effect::LutEffect bad(2, std::vector<float>{1.0f, 0.0f, 0.0f});
        CHECK_FALSE(bad.valid());

        /* cube_size<2. */
        me::effect::LutEffect zero(0, std::vector<float>{});
        CHECK_FALSE(zero.valid());
    });
}

#endif  // ME_HAS_GPU
