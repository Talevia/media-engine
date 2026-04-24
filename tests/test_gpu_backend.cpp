/*
 * test_gpu_backend — contract pins for me::gpu::GpuBackend + the
 * NullGpuBackend default factory return. Phase-1 (null-only):
 * factory returns non-null, backend reports unavailable, name is
 * the expected identifier string.
 */
#include <doctest/doctest.h>

#include "gpu/gpu_backend.hpp"

#include <string>

TEST_CASE("make_gpu_backend: returns a non-null backend") {
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
}

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
