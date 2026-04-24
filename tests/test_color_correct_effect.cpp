/*
 * test_color_correct_effect — pixel-level proof the
 * `me::effect::ColorCorrectEffect` fragment shader runs and
 * produces the expected per-pixel math.
 *
 * Only built + run when `-DME_WITH_GPU=ON`. All bgfx calls and
 * the effect's ctor/dtor run on BgfxGpuBackend's RenderThread
 * (via submit_on_render_thread) per bgfx's single-API-thread
 * contract. Skips cleanly on non-Metal renderers — shader
 * bytecode today is Metal-only.
 *
 * Coverage:
 *   - Identity params (brightness=0, contrast=1, saturation=1)
 *     pass red input through unchanged.
 *   - brightness=+0.25 lifts a mid-gray (0.5) input toward 0.75.
 *   - saturation=0 desaturates red toward Rec-709 luma
 *     (0.2126 * 1.0 ≈ 0x36 on 8-bit scale).
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "effect/color_correct_effect.hpp"
#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t W = 4;
constexpr uint16_t H = 4;

/* Fill a WxHx4 byte buffer with a single RGBA color. */
std::vector<uint8_t> solid_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> buf(static_cast<std::size_t>(W) * H * 4);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = r;
        buf[i + 1] = g;
        buf[i + 2] = b;
        buf[i + 3] = a;
    }
    return buf;
}

struct TestTextures {
    bgfx::TextureHandle     src = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     rt  = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     rb  = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle fb  = BGFX_INVALID_HANDLE;

    ~TestTextures() {
        if (bgfx::isValid(fb))  bgfx::destroy(fb);
        if (bgfx::isValid(rt))  bgfx::destroy(rt);
        if (bgfx::isValid(rb))  bgfx::destroy(rb);
        if (bgfx::isValid(src)) bgfx::destroy(src);
    }
};

/* Pump bgfx::frame() until the readback texture's CPU-side data is
 * populated; bgfx returns the target frame number from
 * bgfx::readTexture and the read becomes visible by that frame. */
void pump_until(uint32_t target) {
    for (int i = 0; i < 10; ++i) {
        if (bgfx::frame() >= target) return;
    }
}

/* Apply `effect` with `src_rgba` as input, render to `W×H`
 * framebuffer, blit to readback texture, return CPU pixels. Assumes
 * called on the render thread. */
std::vector<uint8_t> run_effect_readback(
    const me::effect::ColorCorrectEffect& effect,
    const std::vector<uint8_t>&           src_rgba) {

    TestTextures t;
    /* Source texture — initialized with src_rgba. makeRef keeps a
     * ref to the caller's buffer; bgfx copies on use, so the buffer
     * must stay alive until frame() returns. We have submit_sync
     * scoping so this is fine. */
    const bgfx::Memory* src_mem = bgfx::makeRef(src_rgba.data(), src_rgba.size());
    t.src = bgfx::createTexture2D(
        W, H, /*hasMips=*/false, /*numLayers=*/1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        src_mem);
    REQUIRE(bgfx::isValid(t.src));

    t.rt = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
    t.rb = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
    t.fb = bgfx::createFrameBuffer(1, &t.rt, /*destroyTextures=*/false);
    REQUIRE(bgfx::isValid(t.rt));
    REQUIRE(bgfx::isValid(t.rb));
    REQUIRE(bgfx::isValid(t.fb));

    /* Set viewport rect for view 0 (the effect's draw target);
     * view 1 does the blit. */
    bgfx::setViewRect(0, 0, 0, W, H);
    effect.submit(/*view_id=*/0, t.src, t.fb);
    bgfx::blit(/*view_id=*/1, t.rb, 0, 0, t.rt, 0, 0, W, H);

    std::vector<uint8_t> pixels(static_cast<std::size_t>(W) * H * 4, 0);
    const uint32_t ready = bgfx::readTexture(t.rb, pixels.data());
    pump_until(ready);

    return pixels;
}

bool skip_non_metal() {
    if (bgfx::getRendererType() != bgfx::RendererType::Metal) {
        MESSAGE("Non-Metal renderer — color-correct shader bytecode is "
                "Metal-only today; skipping.");
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE("ColorCorrectEffect: identity params preserve red input") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::ColorCorrectEffect effect;
        REQUIRE(effect.valid());

        const auto red_in = solid_rgba(0xFF, 0x00, 0x00, 0xFF);
        const auto pixels = run_effect_readback(effect, red_in);

        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        CHECK(pixels[mid + 0] == 0xFF);
        CHECK(pixels[mid + 1] == 0x00);
        CHECK(pixels[mid + 2] == 0x00);
        CHECK(pixels[mid + 3] == 0xFF);
    });
}

TEST_CASE("ColorCorrectEffect: brightness lift raises gray toward white") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::ColorCorrectEffect::Params p;
        p.brightness = 0.25f;
        me::effect::ColorCorrectEffect effect(p);
        REQUIRE(effect.valid());

        const auto gray_in = solid_rgba(0x80, 0x80, 0x80, 0xFF);  // ≈0.5
        const auto pixels  = run_effect_readback(effect, gray_in);

        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        /* 0x80 / 255 ≈ 0.502; +0.25 = 0.752; *255 ≈ 0xC0.
         * GPU float precision + potential sampling filter / conversion
         * introduces ±2 ULP wobble; allow a tolerance. */
        auto roughly = [](int v, int target) {
            return v >= target - 3 && v <= target + 3;
        };
        CHECK(roughly(pixels[mid + 0], 0xC0));
        CHECK(roughly(pixels[mid + 1], 0xC0));
        CHECK(roughly(pixels[mid + 2], 0xC0));
        CHECK(pixels[mid + 3] == 0xFF);
    });
}

TEST_CASE("ColorCorrectEffect: saturation=0 desaturates red toward luma") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::ColorCorrectEffect::Params p;
        p.saturation = 0.0f;
        me::effect::ColorCorrectEffect effect(p);
        REQUIRE(effect.valid());

        const auto red_in = solid_rgba(0xFF, 0x00, 0x00, 0xFF);
        const auto pixels = run_effect_readback(effect, red_in);

        /* Pure red → Rec-709 luma = 0.2126 → ≈0x36 on 8-bit. All
         * three channels converge on luma. */
        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        auto roughly = [](int v, int target) {
            return v >= target - 3 && v <= target + 3;
        };
        CHECK(roughly(pixels[mid + 0], 0x36));
        CHECK(roughly(pixels[mid + 1], 0x36));
        CHECK(roughly(pixels[mid + 2], 0x36));
        CHECK(pixels[mid + 3] == 0xFF);
    });
}

#endif  // ME_HAS_GPU
