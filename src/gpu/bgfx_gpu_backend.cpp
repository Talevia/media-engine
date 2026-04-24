#include "gpu/bgfx_gpu_backend.hpp"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <cstdio>
#include <cstring>
#include <utility>

namespace me::gpu {

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
     * refuse headless init. */
    render_thread_->submit_sync([this] {
        bgfx::Init init;
        init.type                = bgfx::RendererType::Count;
        init.platformData.nwh    = nullptr;
        init.resolution.width    = 0;
        init.resolution.height   = 0;
        init.resolution.reset    = BGFX_RESET_NONE;

        init_ok_ = bgfx::init(init);
        if (!init_ok_) {
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
