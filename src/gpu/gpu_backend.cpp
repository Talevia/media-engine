#include "gpu/gpu_backend.hpp"

#include "gpu/null_gpu_backend.hpp"
#ifdef ME_HAS_GPU
#include "gpu/bgfx_gpu_backend.hpp"
#endif

#include <memory>

namespace me::gpu {

std::unique_ptr<GpuBackend> make_gpu_backend() {
#ifdef ME_HAS_GPU
    /* ME_WITH_GPU=ON build: BgfxGpuBackend ctor calls `bgfx::init`
     * headless (Metal auto-picked on macOS; Noop fallback if that
     * fails). `available()` reflects whether init succeeded; caller
     * must always check before issuing GPU work. */
    return std::make_unique<BgfxGpuBackend>();
#else
    /* Phase-1 default: CPU-only. `available() == false` keeps every
     * compose kernel on the existing software path. */
    return std::make_unique<NullGpuBackend>();
#endif
}

}  // namespace me::gpu
