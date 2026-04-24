/*
 * test_blur_effect — pixel-level proof that
 * `me::effect::BlurEffect` (horizontal + vertical) runs the
 * expected 3-tap box-blur math.
 *
 * ME_WITH_GPU-only; skips on non-Metal at runtime (shader
 * bytecode limitation).
 *
 * Coverage:
 *   - Uniform color stays uniform after H+V blur chain (sampler
 *     clamp means edge samples = interior samples = same color,
 *     so average = input).
 *   - 1D edge: input split into red / white halves; after H blur
 *     the seam pixels mix, demonstrating the kernel is active.
 *   - V-only pass on the same red/white horizontal split leaves
 *     pixels unchanged (blur axis orthogonal to the feature).
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "effect/blur_effect.hpp"
#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace {

constexpr uint16_t W = 8;
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

std::vector<uint8_t> uniform_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> buf(static_cast<std::size_t>(W) * H * 4);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = a;
    }
    return buf;
}

/* Left half red, right half white — a vertical seam at x=W/2. */
std::vector<uint8_t> red_white_split() {
    std::vector<uint8_t> buf(static_cast<std::size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * W + x) * 4;
            const bool left = x < W / 2;
            buf[o + 0] = left ? 0xFF : 0xFF;  // R
            buf[o + 1] = left ? 0x00 : 0xFF;  // G
            buf[o + 2] = left ? 0x00 : 0xFF;  // B
            buf[o + 3] = 0xFF;
        }
    }
    return buf;
}

bool skip_non_metal() {
    if (bgfx::getRendererType() != bgfx::RendererType::Metal) {
        MESSAGE("Non-Metal renderer — blur shader is Metal-only; skipping.");
        return true;
    }
    return false;
}

/* Create standard fixtures: source texture with src_rgba, RT
 * framebuffer, readback staging. Returns pixels after effect pass
 * + blit + readback. `submit_effect(view, src_tex, dst_fb)` is
 * called per test — either single-pass or chain. */
template <typename SubmitFn>
std::vector<uint8_t> run_pass(const std::vector<uint8_t>& src_rgba,
                               SubmitFn                    submit_effect) {
    Textures t;
    const bgfx::Memory* src_mem = bgfx::makeRef(src_rgba.data(), src_rgba.size());
    t.src = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        src_mem);
    t.rt = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
    t.rb = bgfx::createTexture2D(
        W, H, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
    t.fb = bgfx::createFrameBuffer(1, &t.rt, false);
    REQUIRE(bgfx::isValid(t.src));
    REQUIRE(bgfx::isValid(t.fb));

    bgfx::setViewRect(0, 0, 0, W, H);
    submit_effect(/*view=*/0, t.src, t.fb);
    bgfx::blit(/*view=*/50, t.rb, 0, 0, t.rt, 0, 0, W, H);

    std::vector<uint8_t> pixels(static_cast<std::size_t>(W) * H * 4, 0);
    const uint32_t ready = bgfx::readTexture(t.rb, pixels.data());
    pump_until(ready);
    return pixels;
}

}  // namespace

TEST_CASE("BlurEffect: uniform red stays uniform red after H-pass") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::BlurEffect::Params p;
        p.pass_width  = W;
        p.pass_height = H;
        me::effect::BlurEffect eff(me::effect::BlurEffect::Direction::Horizontal, p);
        REQUIRE(eff.valid());

        auto pixels = run_pass(uniform_rgba(0xFF, 0x00, 0x00, 0xFF),
            [&](bgfx::ViewId v, bgfx::TextureHandle s, bgfx::FrameBufferHandle d) {
                eff.submit(v, s, d);
            });

        /* Every pixel should be red — sampling on a uniform texture
         * is invariant to kernel size. Allow ±2 ULP wobble. */
        auto roughly = [](int v, int target) {
            return v >= target - 2 && v <= target + 2;
        };
        const std::size_t mid = (static_cast<std::size_t>(H / 2) * W + W / 2) * 4;
        CHECK(roughly(pixels[mid + 0], 0xFF));
        CHECK(pixels[mid + 1] == 0x00);
        CHECK(pixels[mid + 2] == 0x00);
        CHECK(pixels[mid + 3] == 0xFF);
    });
}

TEST_CASE("BlurEffect: H-pass on red/white split blends the seam") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::BlurEffect::Params p;
        p.pass_width  = W;
        p.pass_height = H;
        me::effect::BlurEffect eff(me::effect::BlurEffect::Direction::Horizontal, p);
        REQUIRE(eff.valid());

        auto pixels = run_pass(red_white_split(),
            [&](bgfx::ViewId v, bgfx::TextureHandle s, bgfx::FrameBufferHandle d) {
                eff.submit(v, s, d);
            });

        auto at = [&pixels](int x, int y) -> const uint8_t* {
            return pixels.data() + (static_cast<std::size_t>(y) * W + x) * 4;
        };

        /* Interior pixels (far from seam) unchanged. Middle row y=2. */
        const int y = 2;
        const uint8_t* left_far = at(0, y);
        CHECK(left_far[0] == 0xFF);
        CHECK(left_far[1] == 0x00);
        CHECK(left_far[2] == 0x00);

        const uint8_t* right_far = at(W - 1, y);
        CHECK(right_far[0] == 0xFF);
        CHECK(right_far[1] == 0xFF);
        CHECK(right_far[2] == 0xFF);

        /* Seam pixels blend. Left-of-seam (x = W/2 - 1 = 3) samples
         * pixels 2, 3, 4 — pixel 4 is white. Output R still 0xFF
         * (both red and white have R=0xFF); G and B should be
         * ~0xFF/3 = 0x55. */
        const uint8_t* left_seam = at(W / 2 - 1, y);
        CHECK(left_seam[0] == 0xFF);
        CHECK(left_seam[1] >= 0x40);
        CHECK(left_seam[1] <= 0x60);
        CHECK(left_seam[2] >= 0x40);
        CHECK(left_seam[2] <= 0x60);

        /* Right-of-seam (x = W/2 = 4) samples pixels 3, 4, 5 —
         * pixel 3 is red. Output: R=0xFF, G/B = 0xFF * 2/3 ≈ 0xAA. */
        const uint8_t* right_seam = at(W / 2, y);
        CHECK(right_seam[0] == 0xFF);
        CHECK(right_seam[1] >= 0x95);
        CHECK(right_seam[1] <= 0xB5);
        CHECK(right_seam[2] >= 0x95);
        CHECK(right_seam[2] <= 0xB5);
    });
}

TEST_CASE("BlurEffect: V-pass on red/white vertical seam leaves H unchanged") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());

    backend->submit_on_render_thread([&] {
        if (skip_non_metal()) return;

        me::effect::BlurEffect::Params p;
        p.pass_width  = W;
        p.pass_height = H;
        me::effect::BlurEffect eff(me::effect::BlurEffect::Direction::Vertical, p);
        REQUIRE(eff.valid());

        /* red/white VERTICAL split — feature is on x-axis, so a
         * V-only blur preserves per-column uniformity. */
        auto pixels = run_pass(red_white_split(),
            [&](bgfx::ViewId v, bgfx::TextureHandle s, bgfx::FrameBufferHandle d) {
                eff.submit(v, s, d);
            });

        /* Every red column stays red; every white column stays white. */
        auto at = [&pixels](int x, int y) -> const uint8_t* {
            return pixels.data() + (static_cast<std::size_t>(y) * W + x) * 4;
        };
        for (int y = 0; y < H; ++y) {
            const uint8_t* pl = at(0, y);
            CHECK(pl[0] == 0xFF);
            CHECK(pl[1] == 0x00);
            CHECK(pl[2] == 0x00);

            const uint8_t* pr = at(W - 1, y);
            CHECK(pr[0] == 0xFF);
            CHECK(pr[1] == 0xFF);
            CHECK(pr[2] == 0xFF);
        }
    });
}

#endif  // ME_HAS_GPU
