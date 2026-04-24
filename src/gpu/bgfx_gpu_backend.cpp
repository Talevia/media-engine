#include "gpu/bgfx_gpu_backend.hpp"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <cstdio>
#include <cstring>

namespace me::gpu {

BgfxGpuBackend::BgfxGpuBackend() {
    /* Headless init. With `platformData.nwh = nullptr` bgfx enters
     * its documented headless mode (`Context::init` in bgfx.cpp:
     * `m_headless = (type != Noop && nwh == null && context == null
     * && backBuffer == null && backBufferDS == null)`). Headless
     * REQUIRES `resolution.{width,height} = 0` — any non-zero value
     * trips the early-return "resolution of non-existing backbuffer
     * can't be larger than 0x0". Missed this on first pass; fixed
     * here, confirmed via a scratch probe that Metal init returns
     * true on macOS arm64 with the 0×0 headless config.
     *
     * Scope boundary: we exercise the full init → view-0 set-clear
     * → touch → frame → shutdown sequence. No real backbuffer
     * surface exists (no swap chain), so "clear" is a command-list
     * entry rather than a visible pixel write. Effect-gpu cycles
     * that actually need a drawable framebuffer create one via
     * `bgfx::createFrameBuffer` sized to the composition canvas —
     * orthogonal to this skeleton.
     *
     * RendererType::Count auto-picks: Metal on Apple, Vulkan on
     * Linux, D3D12 on Windows. The Noop retry covers environments
     * where the chosen renderer refuses headless init (unlikely on
     * dev macOS now that the resolution bug is fixed, but still
     * safe for CI matrices with esoteric drivers). */
    bgfx::Init init;
    init.type                = bgfx::RendererType::Count;
    init.platformData.nwh    = nullptr;
    init.resolution.width    = 0;
    init.resolution.height   = 0;
    init.resolution.reset    = BGFX_RESET_NONE;

    init_ok_ = bgfx::init(init);
    if (!init_ok_) {
        /* Auto-pick failed (likely: renderer refused headless init).
         * Retry with Noop — guarantees success on any platform. */
        init.type = bgfx::RendererType::Noop;
        init_ok_  = bgfx::init(init);
    }

    if (init_ok_) {
        const bgfx::RendererType::Enum rt = bgfx::getRendererType();
        const char* rname = bgfx::getRendererName(rt);
        std::snprintf(name_, sizeof(name_), "bgfx-%s", rname);

        /* Exercise the backbuffer: register view 0, queue a clear,
         * force a frame flush. `touch` ensures view 0 is submitted
         * even with no draw calls. */
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x000000ff);
        bgfx::touch(0);
        bgfx::frame();
    } else {
        std::strncpy(name_, "bgfx-failed", sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
    }
}

BgfxGpuBackend::~BgfxGpuBackend() {
    if (init_ok_) bgfx::shutdown();
}

}  // namespace me::gpu
