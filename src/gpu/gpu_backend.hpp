/*
 * me::gpu::GpuBackend — abstract GPU backend interface.
 *
 * Scope-A slice of `bgfx-integration-skeleton` (M3 exit criterion
 * "bgfx 集成，macOS Metal 后端可渲染"). First meaningful step: lay
 * the C++ interface the future Metal/bgfx backend will satisfy.
 * NullGpuBackend (the default factory return) reports
 * `available() == false` so caller code paths (currently just
 * ComposeSink's CPU route) stay the same.
 *
 * When the follow-up cycle adds bgfx FetchContent + Metal init
 * under `ME_WITH_GPU=ON`, it supplies a `BgfxGpuBackend` subclass
 * whose `available()` returns true and adds backend-specific
 * methods (create_texture, frame_begin/end, etc.). The factory
 * `make_gpu_backend()` then chooses between Null and Bgfx based
 * on the compile-time flag.
 *
 * Contract (phase-1 null-only):
 *   - `available()` is the sole method. false = CPU fallback only;
 *     caller-owned and safe to call any number of times.
 *   - Ownership: factory returns unique_ptr; caller stores for
 *     engine lifetime.
 *   - Thread-safety: `available()` is const + stateless, safe
 *     concurrent.
 *
 * Not yet integrated into me_engine or ComposeSink — lands in
 * its own bullet once BgfxGpuBackend exists with enough surface
 * to be meaningful (texture upload + frame submit at minimum).
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

/* Factory. Always returns a non-null backend (NullGpuBackend
 * unless the build was configured with `-DME_WITH_GPU=ON` AND
 * the bgfx FetchContent + real backend lands in a later cycle).
 * Today: always NullGpuBackend. */
std::unique_ptr<GpuBackend> make_gpu_backend();

}  // namespace me::gpu
