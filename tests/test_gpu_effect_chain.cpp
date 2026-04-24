/*
 * test_gpu_effect_chain — skeleton coverage for me::effect::GpuEffect
 * + GpuEffectChain, using NoopGpuEffect as the placeholder effect
 * that exercises view / framebuffer plumbing without a real shader.
 *
 * Only compiled + run when `-DME_WITH_GPU=ON`. All bgfx::* calls
 * (including those inside GpuEffectChain::submit) are routed
 * through `backend->submit_on_render_thread(lambda)` per the
 * single-API-thread contract that landed with
 * bgfx-render-thread-pin.
 *
 * Coverage targets:
 *   - Empty chain: append-less submit is a clean no-op.
 *   - append() takes ownership, size() / kind_at() reflect it.
 *   - N = 1: NoopGpuEffect writes black to dst; readback confirms.
 *   - N = 2: two Noop passes exercise the ping-pong fork — first
 *     pass to scratch_a, second pass to dst. Dst readback still
 *     black (Noop clears everything to black).
 *   - N = 3: three passes, scratch_a → scratch_b → dst, same
 *     black readback.
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "effect/gpu_effect_chain.hpp"
#include "effect/noop_gpu_effect.hpp"
#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

constexpr uint16_t W = 32;
constexpr uint16_t H = 32;

struct FbTriple {
    bgfx::TextureHandle     rt_tex = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle fb     = BGFX_INVALID_HANDLE;

    FbTriple() = default;
    FbTriple(const FbTriple&)            = delete;
    FbTriple& operator=(const FbTriple&) = delete;
    FbTriple(FbTriple&& other) noexcept
        : rt_tex(other.rt_tex), fb(other.fb) {
        other.rt_tex = BGFX_INVALID_HANDLE;
        other.fb     = BGFX_INVALID_HANDLE;
    }
    FbTriple& operator=(FbTriple&& other) noexcept {
        if (this != &other) {
            if (bgfx::isValid(fb))     bgfx::destroy(fb);
            if (bgfx::isValid(rt_tex)) bgfx::destroy(rt_tex);
            rt_tex = other.rt_tex;
            fb     = other.fb;
            other.rt_tex = BGFX_INVALID_HANDLE;
            other.fb     = BGFX_INVALID_HANDLE;
        }
        return *this;
    }

    static FbTriple make(uint64_t extra_flags = 0) {
        FbTriple t;
        t.rt_tex = bgfx::createTexture2D(
            W, H, false, 1,
            bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | extra_flags);
        t.fb = bgfx::createFrameBuffer(1, &t.rt_tex, /*destroyTextures=*/false);
        return t;
    }

    ~FbTriple() {
        if (bgfx::isValid(fb))     bgfx::destroy(fb);
        if (bgfx::isValid(rt_tex)) bgfx::destroy(rt_tex);
    }
};

void pump_until(uint32_t target) {
    for (int i = 0; i < 10; ++i) {
        if (bgfx::frame() >= target) return;
    }
}

/* Shared chain-of-N-Noop pipeline + readback check. All bgfx state
 * allocation, chain submission, blit, readTexture, and handle
 * destruction happen inside the render-thread lambda so bgfx's
 * single-API-thread contract holds. */
void run_noop_chain_case(me::gpu::GpuBackend* backend,
                         std::size_t          n_effects,
                         bgfx::ViewId         first_view_id,
                         bgfx::ViewId         blit_view_id) {
    backend->submit_on_render_thread([&] {
        if (bgfx::getRendererType() == bgfx::RendererType::Noop) {
            MESSAGE("Noop renderer — skipping pixel readback.");
            return;
        }
        const uint32_t caps = bgfx::getCaps()->supported;
        if (!(caps & BGFX_CAPS_TEXTURE_READ_BACK) ||
            !(caps & BGFX_CAPS_TEXTURE_BLIT)) {
            MESSAGE("READ_BACK or BLIT caps missing — skipping.");
            return;
        }

        auto src       = FbTriple::make(BGFX_TEXTURE_BLIT_DST);
        auto dst       = FbTriple::make(BGFX_TEXTURE_BLIT_DST);
        auto scratch_a = FbTriple::make(BGFX_TEXTURE_BLIT_DST);
        auto scratch_b = FbTriple::make(BGFX_TEXTURE_BLIT_DST);
        bgfx::TextureHandle rb_tex = bgfx::createTexture2D(
            W, H, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
        REQUIRE(bgfx::isValid(src.fb));
        REQUIRE(bgfx::isValid(dst.fb));
        REQUIRE(bgfx::isValid(rb_tex));

        me::effect::GpuEffectChain chain;
        for (std::size_t i = 0; i < n_effects; ++i) {
            chain.append(std::make_unique<me::effect::NoopGpuEffect>());
        }
        CHECK(chain.size() == n_effects);

        chain.submit(first_view_id, W, H,
                     src.rt_tex, dst.fb,
                     scratch_a.fb, scratch_b.fb);

        bgfx::blit(blit_view_id, rb_tex, 0, 0, dst.rt_tex, 0, 0, W, H);

        std::vector<uint8_t> pixels(static_cast<std::size_t>(W) * H * 4, 0xFF);
        const uint32_t ready = bgfx::readTexture(rb_tex, pixels.data());
        pump_until(ready);

        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        CHECK(pixels[mid + 0] == 0x00);  // R
        CHECK(pixels[mid + 1] == 0x00);  // G
        CHECK(pixels[mid + 2] == 0x00);  // B
        CHECK(pixels[mid + 3] == 0xFF);  // A (Noop clears with 0x000000ff)

        bgfx::destroy(rb_tex);
    });
}

}  // namespace

TEST_CASE("GpuEffectChain: empty chain submit is a clean no-op") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&backend] {
        me::effect::GpuEffectChain chain;
        CHECK(chain.empty());
        CHECK(chain.size() == 0);

        /* N=0: chain.submit early-returns, no bgfx state touched;
         * the invalid handles are never consulted. */
        chain.submit(0, W, H,
                     BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE,
                     BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE);
        bgfx::frame();

        (void)backend;  // quiet lambda-capture warning on some compilers
    });
}

TEST_CASE("GpuEffectChain: append takes ownership, size + kind_at reflect") {
    /* Pure C++ state manipulation — no bgfx calls, no render thread
     * needed. The backend is still instantiated so we exercise the
     * BgfxGpuBackend lifecycle (ctor + RenderThread startup). */
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);

    me::effect::GpuEffectChain chain;
    chain.append(std::make_unique<me::effect::NoopGpuEffect>());
    chain.append(std::make_unique<me::effect::NoopGpuEffect>());

    CHECK_FALSE(chain.empty());
    CHECK(chain.size() == 2);
    CHECK(chain.kind_at(0) != nullptr);
    CHECK(std::string{chain.kind_at(0)} == "noop-gpu");
    CHECK(std::string{chain.kind_at(1)} == "noop-gpu");
    CHECK(chain.kind_at(2) == nullptr);
}

TEST_CASE("GpuEffectChain: N=1 Noop passthrough, dst reads back black") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());
    run_noop_chain_case(backend.get(), /*n=*/1, /*first_view=*/0, /*blit_view=*/1);
}

TEST_CASE("GpuEffectChain: N=2 ping-pong scratch_a → dst") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());
    run_noop_chain_case(backend.get(), /*n=*/2, /*first_view=*/10, /*blit_view=*/20);
}

TEST_CASE("GpuEffectChain: N=3 ping-pong scratch_a → scratch_b → dst") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());
    run_noop_chain_case(backend.get(), /*n=*/3, /*first_view=*/30, /*blit_view=*/40);
}

#endif  // ME_HAS_GPU
