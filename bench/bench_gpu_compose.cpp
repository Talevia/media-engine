/*
 * bench_gpu_compose — 1080p @ 60fps regression bench.
 *
 * Closes M3 exit criterion "1080p@60 可实时渲染带 3-5 个 GPU
 * effect 的 timeline". Runs a chain of { ColorCorrect +
 * BlurH + BlurV + Lut } on a 1920×1080 RGBA8 framebuffer N
 * times, measures wall-clock time per frame, asserts avg
 * frame time under the 16.67 ms (60 fps) budget.
 *
 * Design notes:
 *   - The bench is a standalone executable (not a doctest
 *     suite) — direct main() keeps output terse + lets CI
 *     parse the ms-per-frame line. Exit code is 0 on budget
 *     met, 1 otherwise.
 *   - ME_WITH_GPU=ON + ME_BUILD_BENCH=ON gate the build. On
 *     non-Metal renderers the bench exits cleanly with a
 *     "skip" message and code 0 (no regression signal).
 *   - Default iteration count is 60 frames (1 second of
 *     real-time budget). Override via `ME_BENCH_FRAMES` env
 *     var for longer release runs.
 *   - Submission path: `backend->submit_on_render_thread` per
 *     frame so bgfx's single-API-thread contract stays intact.
 *     Each submit_sync call blocks until the worker drains the
 *     submitted work, which is the natural synchronization
 *     point for per-frame timing.
 *
 * Honest caveat: bgfx::frame() queues commands to the internal
 * render thread; GPU completion is asynchronous. The wall-clock
 * time we measure here is the API-thread throughput, not GPU
 * completion latency. For Metal on headless macOS this is
 * typically a tight upper bound on actual render time since the
 * command queue is bound — but a high-end pipeline could see
 * GPU-queued work outrun the measurement, overstating speed.
 * Adequate for the "does it hit 60fps ballpark" regression
 * gate; not adequate for frame-pacing validation.
 */
#ifdef ME_HAS_GPU

#include "effect/blur_effect.hpp"
#include "effect/color_correct_effect.hpp"
#include "effect/gpu_effect_chain.hpp"
#include "effect/lut_effect.hpp"
#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

constexpr uint16_t W = 1920;
constexpr uint16_t H = 1080;
constexpr double  BUDGET_MS_PER_FRAME = 1000.0 / 60.0;  // 16.666...

int frames_from_env(int fallback) {
    const char* s = std::getenv("ME_BENCH_FRAMES");
    if (!s || !*s) return fallback;
    const long n = std::strtol(s, nullptr, 10);
    if (n < 1 || n > 10000) return fallback;
    return static_cast<int>(n);
}

std::vector<float> identity_2x2x2_lut() {
    return {
        0, 0, 0,  1, 0, 0,
        0, 1, 0,  1, 1, 0,
        0, 0, 1,  1, 0, 1,
        0, 1, 1,  1, 1, 1,
    };
}

}  // namespace

int main() {
    auto backend = me::gpu::make_gpu_backend();
    if (!backend || !backend->available()) {
        std::fprintf(stderr, "bench_gpu_compose: no GPU backend available; skipping.\n");
        return 0;
    }

    const int n_frames = frames_from_env(60);
    double ms_per_frame = 0.0;
    bool   did_run      = false;

    backend->submit_on_render_thread([&] {
        if (bgfx::getRendererType() == bgfx::RendererType::Noop) {
            std::fprintf(stderr,
                "bench_gpu_compose: Noop renderer — skipping.\n");
            return;
        }

        /* Create 1080p source. Fill with a mid-gray so effects
         * have non-trivial math (brightness +0.1 is measurable). */
        std::vector<uint8_t> src_rgba(
            static_cast<std::size_t>(W) * H * 4, 0x80);
        for (std::size_t i = 3; i < src_rgba.size(); i += 4) {
            src_rgba[i] = 0xFF;
        }

        const bgfx::Memory* src_mem = bgfx::copy(
            src_rgba.data(), static_cast<uint32_t>(src_rgba.size()));
        bgfx::TextureHandle src_tex = bgfx::createTexture2D(
            W, H, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            src_mem);

        bgfx::TextureHandle dst_tex = bgfx::createTexture2D(
            W, H, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
        bgfx::TextureHandle sa_tex = bgfx::createTexture2D(
            W, H, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
        bgfx::TextureHandle sb_tex = bgfx::createTexture2D(
            W, H, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST);
        bgfx::FrameBufferHandle dst_fb = bgfx::createFrameBuffer(1, &dst_tex, false);
        bgfx::FrameBufferHandle sa_fb  = bgfx::createFrameBuffer(1, &sa_tex,  false);
        bgfx::FrameBufferHandle sb_fb  = bgfx::createFrameBuffer(1, &sb_tex,  false);

        /* Chain: ColorCorrect + BlurH + BlurV + Lut.
         * 4 effects — within the bullet's "3-5 GPU effect" range. */
        me::effect::ColorCorrectEffect::Params ccp;
        ccp.brightness = 0.1f;

        me::effect::BlurEffect::Params blur_p;
        blur_p.pass_width  = W;
        blur_p.pass_height = H;

        me::effect::GpuEffectChain chain;
        chain.append(std::make_unique<me::effect::ColorCorrectEffect>(ccp));
        chain.append(std::make_unique<me::effect::BlurEffect>(
            me::effect::BlurEffect::Direction::Horizontal, blur_p));
        chain.append(std::make_unique<me::effect::BlurEffect>(
            me::effect::BlurEffect::Direction::Vertical, blur_p));
        chain.append(std::make_unique<me::effect::LutEffect>(
            2, identity_2x2x2_lut()));

        /* Warm-up frame — ignore timing. Shader programs get
         * committed to the GPU on first draw; first-frame timing
         * is not representative. */
        chain.submit(0, W, H, src_tex, dst_fb, sa_fb, sb_fb);
        bgfx::frame();

        using clock = std::chrono::high_resolution_clock;
        const auto t0 = clock::now();

        for (int i = 0; i < n_frames; ++i) {
            /* Each iteration reuses 4 view IDs (0..3) for the chain
             * + view 4 free. Views get reset implicitly when
             * setViewFrameBuffer is called again. */
            chain.submit(0, W, H, src_tex, dst_fb, sa_fb, sb_fb);
            bgfx::frame();
        }

        const auto t1 = clock::now();
        const std::chrono::duration<double, std::milli> elapsed = t1 - t0;
        ms_per_frame = elapsed.count() / static_cast<double>(n_frames);
        did_run = true;

        bgfx::destroy(dst_fb);
        bgfx::destroy(sa_fb);
        bgfx::destroy(sb_fb);
        bgfx::destroy(dst_tex);
        bgfx::destroy(sa_tex);
        bgfx::destroy(sb_tex);
        bgfx::destroy(src_tex);
    });

    if (!did_run) return 0;

    std::printf("bench_gpu_compose: 1920x1080, %d frames, "
                "avg %.3f ms/frame (budget %.3f ms for 60 fps), "
                "%s\n",
                n_frames, ms_per_frame, BUDGET_MS_PER_FRAME,
                ms_per_frame <= BUDGET_MS_PER_FRAME ? "OK" : "OVER BUDGET");

    return (ms_per_frame <= BUDGET_MS_PER_FRAME) ? 0 : 1;
}

#else

#include <cstdio>

int main() {
    std::fprintf(stderr,
        "bench_gpu_compose: built without ME_WITH_GPU — nothing to measure.\n");
    return 0;
}

#endif  // ME_HAS_GPU
