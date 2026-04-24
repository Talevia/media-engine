#include "gpu/bgfx_gpu_backend.hpp"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace me::gpu {

namespace {

/* Check `ME_GPU_FORCE_NOOP` env var for a truthy value ("1", "true",
 * "yes", "on"). Case-insensitive. Returns false when unset / empty
 * / explicitly falsy. Used by BgfxGpuBackend ctor to bypass the
 * auto-pick renderer and go straight to Noop — primary consumer is
 * the debt-gpu-backend-noop-fallback-test CI gate so the Noop path
 * gets exercised without needing a driver that actually refuses
 * headless init. */
bool env_force_noop() {
    const char* s = std::getenv("ME_GPU_FORCE_NOOP");
    if (!s || !*s) return false;
    /* Tiny case-insensitive match loop — no locale concerns. */
    auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
    const char* truthy[] = { "1", "true", "yes", "on" };
    for (const char* t : truthy) {
        std::size_t i = 0;
        while (t[i] && s[i] && lc(s[i]) == t[i]) ++i;
        if (t[i] == '\0' && (s[i] == '\0' || s[i] == ' ')) return true;
    }
    return false;
}

}  // namespace

BgfxGpuBackend::BgfxGpuBackend() {
    render_thread_ = std::make_unique<RenderThread>();

    /* bgfx::init runs on the render thread to lock in the "API
     * thread" identity — all subsequent bgfx calls (via
     * submit_on_render_thread) will hit the same thread.
     *
     * Headless mode requires resolution 0×0 (bgfx Context::init
     * early-returns if headless && resolution > 0×0; bgfx.cpp:2121).
     * RendererType::Count auto-picks — Metal on macOS, Vulkan on
     * Linux, D3D12 on Windows. Noop retry covers drivers that
     * refuse headless init.
     *
     * Test hook: `ME_GPU_FORCE_NOOP=1` skips auto-pick entirely +
     * uses Noop directly. Lets CI exercise the Noop branch on
     * machines where auto-pick succeeds (post-5566bea dev macOS).
     * Production callers never set it. */
    const bool force_noop = env_force_noop();
    render_thread_->submit_sync([this, force_noop] {
        bgfx::Init init;
        init.type                = force_noop
                                    ? bgfx::RendererType::Noop
                                    : bgfx::RendererType::Count;
        init.platformData.nwh    = nullptr;
        init.resolution.width    = 0;
        init.resolution.height   = 0;
        init.resolution.reset    = BGFX_RESET_NONE;

        init_ok_ = bgfx::init(init);
        if (!init_ok_ && !force_noop) {
            init.type = bgfx::RendererType::Noop;
            init_ok_  = bgfx::init(init);
        }

        if (init_ok_) {
            const bgfx::RendererType::Enum rt = bgfx::getRendererType();
            const char* rname = bgfx::getRendererName(rt);
            std::snprintf(name_, sizeof(name_), "bgfx-%s", rname);

            /* Exercise the backbuffer to prove the init→frame
             * round-trip works. No real drawable exists in headless
             * mode; this is a command-stream touch. */
            bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x000000ff);
            bgfx::touch(0);
            bgfx::frame();
        } else {
            std::strncpy(name_, "bgfx-failed", sizeof(name_) - 1);
            name_[sizeof(name_) - 1] = '\0';
        }
    });
}

BgfxGpuBackend::~BgfxGpuBackend() {
    /* bgfx::shutdown must run on the same thread as bgfx::init.
     * submit_sync blocks until shutdown completes, then the
     * render_thread_ dtor joins its worker. */
    if (render_thread_ && init_ok_) {
        render_thread_->submit_sync([] {
            bgfx::shutdown();
        });
    }
    render_thread_.reset();
}

void BgfxGpuBackend::submit_on_render_thread(std::function<void()> work) {
    if (!render_thread_) {
        /* Shouldn't happen — ctor always creates it. Defensive:
         * run inline so the caller isn't silently skipped. */
        if (work) work();
        return;
    }
    render_thread_->submit_sync(std::move(work));
}

}  // namespace me::gpu
