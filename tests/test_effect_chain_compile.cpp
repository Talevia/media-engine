/*
 * test_effect_chain_compile — coverage for
 * me::effect::GpuEffectChain::compile() pass-fusion.
 *
 * Closes M3 exit criterion "EffectChain 能把连续 ≥ 2 个像素级
 * effect 合并成单 pass" by exercising the CC+CC → fused pair
 * collapse. Fuse catalog is phase-1 (one pair type); extending
 * it is mechanical + tested-shaped follow-up.
 *
 * ME_WITH_GPU-only; skips on non-Metal at runtime.
 *
 * Coverage:
 *   - Empty chain + single effect: compile is a no-op.
 *   - Two CCs collapse into one fused effect (size 2 → 1, kind
 *     changes from "color-correct" to "fused-color-correct").
 *   - Three CCs collapse left-to-right into fused + unchanged CC
 *     (size 3 → 2; phase-1 is greedy pair-wise, not cascading).
 *   - Non-fusable neighbors (CC + Blur) survive unchanged.
 *   - Fused output matches the two-pass output at the pixel
 *     level, pinning the fused shader's math to the per-stage
 *     shader math.
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "effect/blur_effect.hpp"
#include "effect/color_correct_effect.hpp"
#include "effect/gpu_effect_chain.hpp"
#include "effect/noop_gpu_effect.hpp"
#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

bool skip_non_metal() {
    if (bgfx::getRendererType() != bgfx::RendererType::Metal) {
        MESSAGE("Non-Metal renderer — skipping.");
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE("GpuEffectChain::compile: empty chain is a no-op") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    backend->submit_on_render_thread([&] {
        me::effect::GpuEffectChain chain;
        chain.compile();
        CHECK(chain.empty());
    });
}

TEST_CASE("GpuEffectChain::compile: single effect survives") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::GpuEffectChain chain;
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());
        REQUIRE(chain.size() == 1);

        chain.compile();
        CHECK(chain.size() == 1);
        CHECK(std::string{chain.kind_at(0)} == "color-correct");
    });
}

TEST_CASE("GpuEffectChain::compile: two CCs collapse into one fused") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::ColorCorrectEffect::Params a;
        a.brightness = 0.1f;
        me::effect::ColorCorrectEffect::Params b;
        b.brightness = 0.2f;

        me::effect::GpuEffectChain chain;
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>(a));
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>(b));
        REQUIRE(chain.size() == 2);

        chain.compile();
        CHECK(chain.size() == 1);
        CHECK(std::string{chain.kind_at(0)} == "fused-color-correct");
    });
}

TEST_CASE("GpuEffectChain::compile: three CCs collapse greedily (1 fused + 1)") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::GpuEffectChain chain;
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());
        REQUIRE(chain.size() == 3);

        chain.compile();
        CHECK(chain.size() == 2);
        CHECK(std::string{chain.kind_at(0)} == "fused-color-correct");
        CHECK(std::string{chain.kind_at(1)} == "color-correct");
    });
}

TEST_CASE("GpuEffectChain::compile: non-fusable neighbors unchanged") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::GpuEffectChain chain;
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());
        me::effect::BlurEffect::Params bp;
        bp.pass_width  = 32;
        bp.pass_height = 32;
        chain.append(std::make_unique<me::effect::BlurEffect>(
            me::effect::BlurEffect::Direction::Horizontal, bp));
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());

        chain.compile();
        /* No fuse possible (CC / Blur / CC — no adjacent CC+CC). */
        CHECK(chain.size() == 3);
        CHECK(std::string{chain.kind_at(0)} == "color-correct");
        CHECK(std::string{chain.kind_at(1)} == "blur-h");
        CHECK(std::string{chain.kind_at(2)} == "color-correct");
    });
}

TEST_CASE("GpuEffectChain::compile: idempotent — calling twice doesn't double-fuse") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::GpuEffectChain chain;
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>());

        chain.compile();
        CHECK(chain.size() == 1);
        CHECK(std::string{chain.kind_at(0)} == "fused-color-correct");

        chain.compile();  // Second call
        CHECK(chain.size() == 1);
        CHECK(std::string{chain.kind_at(0)} == "fused-color-correct");
    });
}

/* Functional check: fused result matches sequential-pass result.
 * Build two identical chains of [CC(brightness=0.1), CC(brightness=0.2)],
 * compile one, run both, readback — pixels must match within ULP. */
TEST_CASE("GpuEffectChain::compile: fused output matches two-pass output") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;
        const uint32_t caps = bgfx::getCaps()->supported;
        if (!(caps & BGFX_CAPS_TEXTURE_READ_BACK) ||
            !(caps & BGFX_CAPS_TEXTURE_BLIT)) {
            MESSAGE("READ_BACK or BLIT caps missing — skipping.");
            return;
        }

        constexpr uint16_t W = 4;
        constexpr uint16_t H = 4;

        /* Gray input so brightness arithmetic is well-exercised;
         * neither saturated high nor clipped low. */
        std::vector<uint8_t> src_rgba(
            static_cast<std::size_t>(W) * H * 4);
        for (std::size_t i = 0; i < src_rgba.size(); i += 4) {
            src_rgba[i + 0] = 0x40;
            src_rgba[i + 1] = 0x40;
            src_rgba[i + 2] = 0x40;
            src_rgba[i + 3] = 0xFF;
        }

        /* Helper to run a chain end-to-end and return readback
         * bytes. Each call creates its own src/dst/scratch/rb
         * textures so there's no inter-test framebuffer state. */
        auto run_chain = [&](me::effect::GpuEffectChain& chain,
                             bgfx::ViewId first_view,
                             bgfx::ViewId blit_view) {
            const bgfx::Memory* mem = bgfx::makeRef(
                src_rgba.data(),
                static_cast<uint32_t>(src_rgba.size()));
            bgfx::TextureHandle src = bgfx::createTexture2D(
                W, H, false, 1, bgfx::TextureFormat::RGBA8,
                BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                mem);
            bgfx::TextureHandle rt_dst = bgfx::createTexture2D(
                W, H, false, 1, bgfx::TextureFormat::RGBA8,
                BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
            bgfx::TextureHandle rt_sa = bgfx::createTexture2D(
                W, H, false, 1, bgfx::TextureFormat::RGBA8,
                BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
            bgfx::TextureHandle rt_sb = bgfx::createTexture2D(
                W, H, false, 1, bgfx::TextureFormat::RGBA8,
                BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
            bgfx::TextureHandle rb = bgfx::createTexture2D(
                W, H, false, 1, bgfx::TextureFormat::RGBA8,
                BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);

            bgfx::FrameBufferHandle fb_dst = bgfx::createFrameBuffer(1, &rt_dst, false);
            bgfx::FrameBufferHandle fb_sa  = bgfx::createFrameBuffer(1, &rt_sa,  false);
            bgfx::FrameBufferHandle fb_sb  = bgfx::createFrameBuffer(1, &rt_sb,  false);

            chain.submit(first_view, W, H, src, fb_dst, fb_sa, fb_sb);
            bgfx::blit(blit_view, rb, 0, 0, rt_dst, 0, 0, W, H);

            std::vector<uint8_t> pixels(
                static_cast<std::size_t>(W) * H * 4, 0);
            const uint32_t ready = bgfx::readTexture(rb, pixels.data());
            for (int i = 0; i < 10; ++i) {
                if (bgfx::frame() >= ready) break;
            }

            bgfx::destroy(fb_sb);
            bgfx::destroy(fb_sa);
            bgfx::destroy(fb_dst);
            bgfx::destroy(rb);
            bgfx::destroy(rt_sb);
            bgfx::destroy(rt_sa);
            bgfx::destroy(rt_dst);
            bgfx::destroy(src);
            return pixels;
        };

        me::effect::ColorCorrectEffect::Params pa;
        pa.brightness = 0.1f;
        me::effect::ColorCorrectEffect::Params pb;
        pb.brightness = 0.2f;

        /* Chain A: compiled (fused). */
        me::effect::GpuEffectChain chain_fused;
        chain_fused.append(std::make_unique<me::effect::ColorCorrectEffect>(pa));
        chain_fused.append(std::make_unique<me::effect::ColorCorrectEffect>(pb));
        chain_fused.compile();
        REQUIRE(chain_fused.size() == 1);
        auto fused_px = run_chain(chain_fused, 0, 5);

        /* Chain B: two-pass (no compile). */
        me::effect::GpuEffectChain chain_two;
        chain_two.append(std::make_unique<me::effect::ColorCorrectEffect>(pa));
        chain_two.append(std::make_unique<me::effect::ColorCorrectEffect>(pb));
        REQUIRE(chain_two.size() == 2);
        auto two_px = run_chain(chain_two, 10, 15);

        /* Pixel-wise equivalence. Allow ±3 ULP per channel;
         * two-pass accumulates one more round of 8-bit
         * quantization via the intermediate framebuffer. */
        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        auto roughly = [](int v, int target) {
            return v >= target - 3 && v <= target + 3;
        };
        CHECK(roughly(fused_px[mid + 0], two_px[mid + 0]));
        CHECK(roughly(fused_px[mid + 1], two_px[mid + 1]));
        CHECK(roughly(fused_px[mid + 2], two_px[mid + 2]));
        CHECK(fused_px[mid + 3] == two_px[mid + 3]);
    });
}

#endif  // ME_HAS_GPU
