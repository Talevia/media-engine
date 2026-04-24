#include "gpu/gpu_backend.hpp"

#include "gpu/null_gpu_backend.hpp"

#include <memory>

namespace me::gpu {

std::unique_ptr<GpuBackend> make_gpu_backend() {
    /* Phase-1: always null backend — CPU compose path serves all
     * rendering. Follow-up `bgfx-integration-skeleton` cycle
     * (FetchContent + Metal init) swaps this to return a
     * BgfxGpuBackend under `ME_WITH_GPU=ON` build configuration. */
    return std::make_unique<NullGpuBackend>();
}

}  // namespace me::gpu
