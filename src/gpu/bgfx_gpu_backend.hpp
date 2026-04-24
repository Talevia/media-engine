/*
 * me::gpu::BgfxGpuBackend — bgfx-backed GpuBackend.
 *
 * Compiled only when the build is configured with `-DME_WITH_GPU=ON`
 * (see root `CMakeLists.txt` + `src/CMakeLists.txt`). Constructor
 * performs a headless `bgfx::init` with Metal preferred on macOS
 * (`RendererType::Count` lets bgfx auto-pick on other platforms) and
 * `platformData.nwh = nullptr` — no window surface, render-to-texture
 * only. Resolution is 1×1 because the skeleton only needs a valid
 * backbuffer to satisfy `bgfx::setViewClear` + `bgfx::frame`; future
 * compose cycles (effect-gpu-*) create their own framebuffers sized
 * to the output canvas.
 *
 * Contract:
 *   - `available()` returns true iff `bgfx::init` succeeded in the
 *     ctor. A failed init is not an error — the caller falls through
 *     to the CPU compose path exactly as with `NullGpuBackend`.
 *   - `name()` is "bgfx-<renderer>" on success (e.g. "bgfx-metal"),
 *     "bgfx-failed" otherwise. Renderer string is what the log /
 *     tests assert against.
 *   - Dtor calls `bgfx::shutdown` iff ctor init succeeded. Order
 *     matters: engine-owned unique_ptr<GpuBackend> destructs before
 *     FramePool / Scheduler in me_engine; bgfx has no dependency on
 *     those so the order is fine.
 *
 * Thread-safety: bgfx's own API contract is single-threaded for the
 * "API thread" (the one calling `bgfx::frame`). Engine today uses
 * the backend from only one place (ctor + dtor on main thread); when
 * compose kernels start issuing bgfx calls we'll need to pin them
 * to a dedicated render thread. That's future work.
 */
#pragma once

#include "gpu/gpu_backend.hpp"

namespace me::gpu {

class BgfxGpuBackend final : public GpuBackend {
public:
    BgfxGpuBackend();
    ~BgfxGpuBackend() override;

    BgfxGpuBackend(const BgfxGpuBackend&)            = delete;
    BgfxGpuBackend& operator=(const BgfxGpuBackend&) = delete;

    bool        available() const noexcept override { return init_ok_; }
    const char* name()      const noexcept override { return name_; }

private:
    bool init_ok_ = false;
    /* Stored as a buffer (not std::string) to keep name() noexcept +
     * stable-address for the C-string contract. Long enough for
     * "bgfx-<renderer>" (<32 chars in practice). */
    char name_[32] = "bgfx-uninit";
};

}  // namespace me::gpu
