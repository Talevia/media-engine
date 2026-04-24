/*
 * me::gpu::NullGpuBackend — no-op default GpuBackend.
 *
 * Returns `available() == false` so existing CPU compose paths
 * stay live unchanged. Exists to give the factory a concrete
 * non-null return value before BgfxGpuBackend is implemented —
 * callers can depend on `make_gpu_backend()` always producing
 * a valid pointer and query `available()` for the branch point.
 */
#pragma once

#include "gpu/gpu_backend.hpp"

namespace me::gpu {

class NullGpuBackend final : public GpuBackend {
public:
    bool        available() const noexcept override { return false; }
    const char* name()      const noexcept override { return "null"; }
};

}  // namespace me::gpu
