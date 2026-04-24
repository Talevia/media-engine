/*
 * me::gpu::GpuBackend — abstract GPU backend interface.
 *
 * Two implementations:
 *   - `NullGpuBackend` (header-only, `null_gpu_backend.hpp`): default.
 *     `available() == false`, `name() == "null"`. Every compose
 *     kernel stays on the CPU path.
 *   - `BgfxGpuBackend` (`bgfx_gpu_backend.{hpp,cpp}`): compiled only
 *     when `-DME_WITH_GPU=ON`. Ctor performs a headless `bgfx::init`
 *     (Metal auto-pick on macOS; Noop fallback if the driver refuses
 *     headless init), clears a 1×1 backbuffer, retains context until
 *     dtor → `bgfx::shutdown`. `available() == true` once init
 *     succeeds; `name()` is "bgfx-<renderer>" (e.g. "bgfx-Metal").
 *
 * Contract:
 *   - `available()`: true iff GPU work can be issued. Safe to call
 *     any number of times; stateless.
 *   - `name()`: short identifier for log / debug. Stable for the
 *     backend's lifetime (address is stable C string).
 *   - Ownership: factory returns unique_ptr; caller (me_engine)
 *     stores for engine lifetime. Engine-owned unique_ptr destructs
 *     before other resources in me_engine so bgfx::shutdown runs
 *     with no outstanding GPU work.
 *   - Thread-safety: `available()` / `name()` are const + stateless,
 *     safe concurrent. bgfx itself is single-threaded on the API
 *     thread; compose kernels issuing bgfx calls must pin to a
 *     dedicated render thread (future work — not yet wired).
 */
#pragma once

#include <memory>

namespace me::gpu {

class GpuBackend {
public:
    virtual ~GpuBackend() = default;

    /* True when the backend is functional and GPU-resident
     * resources can be created. Null backend always returns false
     * — caller falls through to CPU compose path. */
    virtual bool available() const noexcept = 0;

    /* Backend identifier (log / debug). "null" for NullGpuBackend;
     * future "bgfx-metal" / "bgfx-vulkan" / "bgfx-dx11" as the
     * bgfx integration lands. */
    virtual const char* name() const noexcept = 0;
};

/* Factory. Always returns a non-null backend: `BgfxGpuBackend` under
 * `-DME_WITH_GPU=ON` builds (when `ME_HAS_GPU` is defined in the TU),
 * `NullGpuBackend` otherwise. Caller must still check `available()`
 * because `BgfxGpuBackend`'s init can fall back to Noop if the driver
 * refuses headless Metal — in that case `available()` is true but the
 * renderer is the Noop no-op driver. */
std::unique_ptr<GpuBackend> make_gpu_backend();

}  // namespace me::gpu
