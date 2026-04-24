/*
 * test_bgfx_metal_render — pixel-level proof that the GPU backend
 * actually renders.
 *
 * Only compiled + run when `-DME_WITH_GPU=ON` (compile-def
 * ME_HAS_GPU is propagated from media_engine). The ME_WITH_GPU=OFF
 * default build skips this file entirely via the outer #ifdef
 * guard, so nothing links against bgfx unless the GPU path is
 * deliberately enabled.
 *
 * Scope — closes M3 exit criterion "bgfx 集成，macOS Metal 后端可
 * 渲染" pixel-proof gap: bgfx-integration-skeleton (5566bea)
 * demonstrated init → clear-command → frame-submit → shutdown,
 * but produced no evidence Metal wrote a single pixel. This test
 * creates a 64×64 RGBA8 framebuffer, clears it red, reads it
 * back, and asserts the bytes. If `bgfx::getRendererType()` is
 * Noop (unlikely on macOS post-0×0-resolution fix) or the driver
 * doesn't expose `BGFX_CAPS_TEXTURE_READ_BACK` (which Noop never
 * does), the test records the skip reason and returns OK —
 * pixel-proof isn't possible under Noop by definition.
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace {

/* Spin bgfx::frame() until readback completes. bgfx::readTexture
 * returns the frame number at which the CPU-side buffer becomes
 * valid; every bgfx::frame() increments the counter. Cap at 10
 * iterations — ~10 ms budget on Metal, plenty for a 64×64 readback;
 * past that we assume the driver never actually filed the readback
 * and the test will fail on the pixel assertion below. */
void pump_until(uint32_t target_frame, int max_spins = 10) {
    for (int i = 0; i < max_spins; ++i) {
        const uint32_t cur = bgfx::frame();
        if (cur >= target_frame) return;
    }
}

}  // namespace

TEST_CASE("bgfx renders a red 64x64 framebuffer and reads it back") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    const bgfx::RendererType::Enum rt = bgfx::getRendererType();
    if (bgfx::RendererType::Noop == rt) {
        MESSAGE("Noop renderer active — pixel-level render proof not "
                "achievable under Noop by definition. Test passes on "
                "drivers that refuse headless init of Metal/Vulkan/"
                "D3D12; dev macOS should be picking Metal post-5566bea.");
        return;
    }

    const uint32_t caps = bgfx::getCaps()->supported;
    if (!(caps & BGFX_CAPS_TEXTURE_READ_BACK)) {
        MESSAGE("BGFX_CAPS_TEXTURE_READ_BACK unsupported on this "
                "renderer — can't read GPU framebuffer back to CPU.");
        return;
    }
    if (!(caps & BGFX_CAPS_TEXTURE_BLIT)) {
        MESSAGE("BGFX_CAPS_TEXTURE_BLIT unsupported on this renderer "
                "— need blit to copy RT → READ_BACK staging texture.");
        return;
    }

    /* 64×64 is small enough to keep readback trivially cheap, big
     * enough that a single-bit off-by-one in buffer sizing would
     * produce obviously-wrong pixels. RGBA8 rather than BGRA8: the
     * in-memory byte order matches setViewClear's documented RGBA
     * uint32_t layout (R=MSB byte), so the readback byte check below
     * doesn't need to channel-swap.
     *
     * Metal (caps-probed 2026-04-24) refuses `RT | READ_BACK` on the
     * same texture — returns an invalid handle. Workaround: two
     * textures joined by blit. `rt_tex` is the render target bound
     * to the framebuffer; `rb_tex` is a plain READ_BACK staging
     * texture that we blit into after clearing the RT. readTexture
     * then reads from `rb_tex`. This is the documented bgfx pattern
     * for headless Metal pixel readback (see bgfx example 17 for
     * the same idiom on other platforms). */
    constexpr uint16_t W = 64;
    constexpr uint16_t H = 64;
    constexpr uint32_t CLEAR_RGBA = 0xFF0000FF;  // R=0xFF G=0x00 B=0x00 A=0xFF

    bgfx::TextureHandle rt_tex = bgfx::createTexture2D(
        W, H, /*hasMips=*/false, /*numLayers=*/1,
        bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
    REQUIRE(bgfx::isValid(rt_tex));

    bgfx::TextureHandle rb_tex = bgfx::createTexture2D(
        W, H, /*hasMips=*/false, /*numLayers=*/1,
        bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
    REQUIRE(bgfx::isValid(rb_tex));

    bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(
        /*_num=*/1, &rt_tex, /*_destroyTextures=*/false);
    REQUIRE(bgfx::isValid(fb));

    /* View 0: clear the RT. View 1: blit RT → RB, execute after
     * view 0. Two views rather than one because bgfx::blit queues
     * on a view, and clearing + blitting in the same view has
     * undefined ordering w.r.t. the clear command. */
    bgfx::setViewFrameBuffer(0, fb);
    bgfx::setViewRect(0, 0, 0, W, H);
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR, CLEAR_RGBA, /*depth=*/1.0f, /*stencil=*/0);
    bgfx::touch(0);
    bgfx::blit(/*viewId=*/1, rb_tex, 0, 0, rt_tex, 0, 0, W, H);

    std::vector<uint8_t> pixels(static_cast<std::size_t>(W) * H * 4, 0);
    const uint32_t ready_frame = bgfx::readTexture(rb_tex, pixels.data());
    pump_until(ready_frame);

    /* Spot-check all four corner pixels. A full-buffer scan would be
     * overkill — the GPU clear is a single command, so any pixel
     * being wrong means the whole buffer is wrong. */
    auto at = [&pixels](int x, int y) -> const uint8_t* {
        return pixels.data() + (static_cast<std::size_t>(y) * W + x) * 4;
    };
    for (auto [x, y] : std::initializer_list<std::pair<int,int>>{
             {0, 0}, {W - 1, 0}, {0, H - 1}, {W - 1, H - 1},
             {W / 2, H / 2}}) {
        const uint8_t* p = at(x, y);
        CHECK(p[0] == 0xFF);  // R
        CHECK(p[1] == 0x00);  // G
        CHECK(p[2] == 0x00);  // B
        CHECK(p[3] == 0xFF);  // A
    }

    bgfx::destroy(fb);
    bgfx::destroy(rt_tex);
    bgfx::destroy(rb_tex);
}

#endif  // ME_HAS_GPU
