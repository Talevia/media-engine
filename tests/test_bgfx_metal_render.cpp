/*
 * test_bgfx_metal_render — pixel-level proof that the GPU backend
 * actually renders.
 *
 * Only compiled + run when `-DME_WITH_GPU=ON`. All bgfx::* calls
 * route through `backend->submit_on_render_thread(lambda)` so
 * bgfx's single-API-thread contract holds: init runs on the
 * RenderThread (from BgfxGpuBackend's ctor), subsequent calls
 * from the test run on the same RenderThread via submit_sync.
 *
 * Scope — closes M3 exit criterion "bgfx 集成，macOS Metal 后端可
 * 渲染" pixel-proof gap: the cycle before bgfx-integration-skeleton
 * + framebuffer-readback demonstrated the sequence on the main
 * thread; this cycle re-asserts the same proof after the
 * bgfx-render-thread-pin refactor moved bgfx ownership to a
 * dedicated thread.
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace {

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

    backend->submit_on_render_thread([] {
        const bgfx::RendererType::Enum rt = bgfx::getRendererType();
        if (bgfx::RendererType::Noop == rt) {
            MESSAGE("Noop renderer active — pixel-level render proof not "
                    "achievable under Noop by definition.");
            return;
        }

        const uint32_t caps = bgfx::getCaps()->supported;
        if (!(caps & BGFX_CAPS_TEXTURE_READ_BACK)) {
            MESSAGE("BGFX_CAPS_TEXTURE_READ_BACK unsupported on this "
                    "renderer — can't read GPU framebuffer back to CPU.");
            return;
        }
        if (!(caps & BGFX_CAPS_TEXTURE_BLIT)) {
            MESSAGE("BGFX_CAPS_TEXTURE_BLIT unsupported — need blit for "
                    "RT → READ_BACK staging copy.");
            return;
        }

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

        bgfx::setViewFrameBuffer(0, fb);
        bgfx::setViewRect(0, 0, 0, W, H);
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR, CLEAR_RGBA,
                           /*depth=*/1.0f, /*stencil=*/0);
        bgfx::touch(0);
        bgfx::blit(/*viewId=*/1, rb_tex, 0, 0, rt_tex, 0, 0, W, H);

        std::vector<uint8_t> pixels(static_cast<std::size_t>(W) * H * 4, 0);
        const uint32_t ready_frame = bgfx::readTexture(rb_tex, pixels.data());
        pump_until(ready_frame);

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
    });
}

#endif  // ME_HAS_GPU
