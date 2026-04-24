/*
 * test_compose_sink_gpu_detect — unit tests for
 * me::orchestrator::is_gpu_compose_usable, the predicate that
 * ComposeSink uses to decide whether a GpuBackend can drive GPU-
 * accelerated compose.
 *
 * Platform-agnostic: uses a small inline MockGpuBackend so the
 * predicate's positive path is exercised even in builds where
 * ME_WITH_GPU=OFF (no bgfx linked; no real BgfxGpuBackend).
 */
#include <doctest/doctest.h>

#include "gpu/gpu_backend.hpp"
#include "orchestrator/compose_sink.hpp"

namespace {

/* Minimal GpuBackend subclass for predicate testing — lets us
 * drive any (available, name) combination without caring about
 * whether bgfx is linked. */
class MockGpuBackend final : public me::gpu::GpuBackend {
public:
    MockGpuBackend(bool available_, const char* name_)
        : available_(available_), name_(name_) {}
    bool        available() const noexcept override { return available_; }
    const char* name()      const noexcept override { return name_; }
private:
    bool        available_;
    const char* name_;
};

}  // namespace

TEST_CASE("is_gpu_compose_usable: null backend is not usable") {
    CHECK_FALSE(me::orchestrator::is_gpu_compose_usable(nullptr));
}

TEST_CASE("is_gpu_compose_usable: unavailable backend is not usable") {
    MockGpuBackend b(/*available=*/false, "bgfx-Metal");
    CHECK_FALSE(me::orchestrator::is_gpu_compose_usable(&b));
}

TEST_CASE("is_gpu_compose_usable: NullGpuBackend (name='null') is not usable") {
    MockGpuBackend b(/*available=*/false, "null");
    CHECK_FALSE(me::orchestrator::is_gpu_compose_usable(&b));
    /* Even if someone flipped available=true on a null-named backend
     * (which wouldn't happen in practice but defends the contract),
     * the name prefix check rejects it. */
    MockGpuBackend b_lying(/*available=*/true, "null");
    CHECK_FALSE(me::orchestrator::is_gpu_compose_usable(&b_lying));
}

TEST_CASE("is_gpu_compose_usable: bgfx-Noop fallback is rejected") {
    /* Noop reports available=true but its draws don't write
     * pixels — treating it as GPU-usable would silently run
     * compose with a renderer that produces no output. Must not. */
    MockGpuBackend b(/*available=*/true, "bgfx-Noop");
    CHECK_FALSE(me::orchestrator::is_gpu_compose_usable(&b));
}

TEST_CASE("is_gpu_compose_usable: real bgfx renderers are usable") {
    /* The renderer name suffix comes from bgfx::getRendererName; we
     * don't enumerate all platforms here, just confirm the "bgfx-<x>"
     * shape is accepted for representative x ∈ {Metal, Vulkan, D3D12,
     * OpenGL}. */
    for (const char* n : {"bgfx-Metal", "bgfx-Vulkan", "bgfx-Direct3D12",
                          "bgfx-OpenGL"}) {
        MockGpuBackend b(/*available=*/true, n);
        CHECK(me::orchestrator::is_gpu_compose_usable(&b));
    }
}

TEST_CASE("is_gpu_compose_usable: unavailable bgfx-Metal still rejected") {
    /* available() gates independently of the name check. A future
     * BgfxGpuBackend that failed to init but still reports its
     * intended renderer name must not be treated as usable. */
    MockGpuBackend b(/*available=*/false, "bgfx-Metal");
    CHECK_FALSE(me::orchestrator::is_gpu_compose_usable(&b));
}

TEST_CASE("is_gpu_compose_usable: null name is rejected") {
    MockGpuBackend b(/*available=*/true, nullptr);
    CHECK_FALSE(me::orchestrator::is_gpu_compose_usable(&b));
}
