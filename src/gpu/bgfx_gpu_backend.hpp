/*
 * me::gpu::BgfxGpuBackend — bgfx-backed GpuBackend.
 *
 * Compiled only when the build is configured with `-DME_WITH_GPU=ON`
 * (see root `CMakeLists.txt` + `src/CMakeLists.txt`). Pins all
 * bgfx::* API calls to a dedicated render thread (me::gpu::
 * RenderThread). The render thread runs bgfx::init in the ctor
 * (via submit_sync → waits for completion), bgfx::shutdown in the
 * dtor, and any future compose-kernel bgfx work via
 * `submit_on_render_thread(lambda)`.
 *
 * Rationale: bgfx's threading contract is "all bgfx calls (except
 * bgfx::frame) come from the same thread — determined by whoever
 * calls bgfx::init". Without a pinned render thread, a future
 * compose kernel running on a scheduler worker thread would call
 * bgfx from thread B while init ran on thread A → undefined
 * behavior. Pinning now lands the invariant before it's tested in
 * production.
 *
 * Contract:
 *   - `available()` returns true iff `bgfx::init` succeeded on the
 *     render thread. A failed init is not an error — the caller
 *     falls through to the CPU compose path exactly as with
 *     `NullGpuBackend`.
 *   - `name()` is "bgfx-<renderer>" on success (e.g. "bgfx-Metal"),
 *     "bgfx-failed" otherwise. Renderer string is what the log /
 *     tests assert against.
 *   - `submit_on_render_thread(work)` routes `work` through the
 *     render thread and blocks until it completes. Rethrows
 *     exceptions the work lambda surfaces (so doctest REQUIRE /
 *     CHECK inside the lambda behave normally). Tests that need
 *     direct bgfx calls (createTexture2D, readTexture, etc.) must
 *     wrap them in this method — calling bgfx from a non-render
 *     thread violates bgfx's contract.
 *   - Dtor submits bgfx::shutdown to the render thread (when init
 *     succeeded), then destroys the RenderThread (which joins its
 *     worker).
 *
 * Ownership: the render thread is owned by this class via
 * unique_ptr. me_engine owns the BgfxGpuBackend via unique_ptr in
 * engine_impl.hpp; engine_destroy → BgfxGpuBackend dtor →
 * RenderThread dtor, in that order.
 */
#pragma once

#include "gpu/gpu_backend.hpp"
#include "gpu/render_thread.hpp"

#include <functional>
#include <memory>

namespace me::gpu {

class BgfxGpuBackend final : public GpuBackend {
public:
    BgfxGpuBackend();
    ~BgfxGpuBackend() override;

    BgfxGpuBackend(const BgfxGpuBackend&)            = delete;
    BgfxGpuBackend& operator=(const BgfxGpuBackend&) = delete;

    bool        available() const noexcept override { return init_ok_; }
    const char* name()      const noexcept override { return name_; }

    /* Run `work` on the bgfx API thread (this backend's RenderThread)
     * and block until it completes. Override of the GpuBackend base
     * method — the ONLY way external code should call bgfx::*
     * functions. Any direct call from another thread violates bgfx's
     * single-API-thread contract. */
    void submit_on_render_thread(std::function<void()> work) override;

private:
    bool init_ok_ = false;
    /* Stored as a buffer (not std::string) to keep name() noexcept +
     * stable-address for the C-string contract. Long enough for
     * "bgfx-<renderer>" (<32 chars in practice). */
    char name_[32] = "bgfx-uninit";

    /* Worker thread that owns the bgfx API thread invariant. Created
     * before bgfx::init (ctor submits init to it via submit_sync);
     * destroyed after bgfx::shutdown. Never exposed directly —
     * callers route work through submit_on_render_thread(). */
    std::unique_ptr<RenderThread> render_thread_;
};

}  // namespace me::gpu
