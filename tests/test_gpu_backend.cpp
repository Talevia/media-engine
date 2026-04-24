/*
 * test_gpu_backend — contract pins for me::gpu::GpuBackend.
 *
 * Phase-1 (ME_WITH_GPU=OFF, default): factory returns NullGpuBackend,
 * `available()` is false, `name()` is "null".
 *
 * Phase-2 (ME_WITH_GPU=ON, bgfx linked): factory returns
 * BgfxGpuBackend, `name()` starts with "bgfx-" (renderer-specific
 * suffix: "bgfx-Metal" on macOS, "bgfx-Noop" on a headless-refusing
 * driver, etc.). We DO NOT assert a specific renderer — that depends
 * on the platform + driver quirks — only the family prefix.
 */
#include <doctest/doctest.h>

#include "gpu/gpu_backend.hpp"

#include <string>

TEST_CASE("make_gpu_backend: returns a non-null backend") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
}

#ifndef ME_HAS_GPU
TEST_CASE("NullGpuBackend (default): reports unavailable") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    CHECK_FALSE(backend->available());
}

TEST_CASE("NullGpuBackend: name identifier is 'null'") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    CHECK(std::string{backend->name()} == "null");
}

TEST_CASE("GpuBackend: available() + name() are thread-safe-callable (smoke)") {
    /* Stateless const getters — repeated calls must yield
     * identical results. Smoke test rather than actual
     * concurrent assertion; proves the interface doesn't
     * accidentally hold state that would preclude future
     * concurrent query from multiple compose threads. */
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    for (int i = 0; i < 100; ++i) {
        CHECK_FALSE(backend->available());
        CHECK(std::string{backend->name()} == "null");
    }
}
#endif  // !ME_HAS_GPU

#ifdef ME_HAS_GPU
TEST_CASE("BgfxGpuBackend: reports available after bgfx::init") {
    /* ME_WITH_GPU=ON build. bgfx::init may pick Metal / Vulkan / D3D
     * or fall back to Noop (ctor's auto→Noop two-stage retry). Either
     * way `available()` is true once construction returns. */
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    CHECK(backend->available());
}

TEST_CASE("BgfxGpuBackend: name has 'bgfx-' prefix") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    const std::string n = backend->name();
    /* Must be "bgfx-<renderer>" on success; "bgfx-failed" if both
     * auto and Noop init fail (not expected in practice since Noop
     * init is unconditional). Either way the prefix is stable. */
    CHECK(n.rfind("bgfx-", 0) == 0);
}

TEST_CASE("BgfxGpuBackend: ctor→dtor cycle is clean (smoke)") {
    /* bgfx::init + bgfx::shutdown is a process-wide singleton — two
     * sequential ctors are valid iff the first dtor runs in between.
     * Smoke-test that sequence works so future engine multi-create
     * paths (tests spinning up and tearing down engines) don't leak
     * bgfx state. */
    for (int i = 0; i < 3; ++i) {
        auto backend = me::gpu::make_gpu_backend();
        REQUIRE(backend);
        CHECK(backend->available());
    }
}
#endif  // ME_HAS_GPU
