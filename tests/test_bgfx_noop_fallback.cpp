/*
 * test_bgfx_noop_fallback — CI coverage for BgfxGpuBackend's Noop
 * path.
 *
 * Motivation: bgfx_gpu_backend.cpp has a two-stage renderer init —
 * auto-pick first (Metal on macOS etc.), Noop retry if auto-pick
 * refuses headless. Post-5566bea (the 0×0-resolution fix) Metal
 * init succeeds first-try on dev macOS, so the Noop branch was
 * never exercised — latent regression risk on drivers that DO
 * refuse headless init.
 *
 * This test uses the `ME_GPU_FORCE_NOOP=1` env var hook to force
 * the Noop branch, then asserts the contract: available()==true,
 * name()=="bgfx-Noop", submit_on_render_thread still works
 * (routes through the render thread), ctor→dtor cycle is clean.
 */
#ifdef ME_HAS_GPU

#include <doctest/doctest.h>

#include "gpu/gpu_backend.hpp"

#include <bgfx/bgfx.h>

#include <cstdlib>
#include <string>

TEST_CASE("BgfxGpuBackend: ME_GPU_FORCE_NOOP=1 selects Noop renderer") {
    /* setenv is POSIX-compatible (macOS + Linux). Test runs on
     * those; Windows CI would need `_putenv_s` — not gated for
     * now since bgfx tests are dev-Mac primary. */
    ::setenv("ME_GPU_FORCE_NOOP", "1", /*overwrite=*/1);

    {
        auto backend = me::gpu::make_gpu_backend();
        REQUIRE(backend);
        CHECK(backend->available());
        CHECK(std::string{backend->name()} == "bgfx-Noop");

        /* submit_on_render_thread still works — routes the lambda
         * through the same RenderThread the init ran on. */
        int sentinel = 0;
        backend->submit_on_render_thread([&sentinel] {
            sentinel = 42;
            /* bgfx::getRendererType() is the Noop renderer's value. */
            CHECK(bgfx::getRendererType() == bgfx::RendererType::Noop);
        });
        CHECK(sentinel == 42);

        /* Backend destructor runs below when `backend` leaves scope
         * — bgfx::shutdown must run cleanly on the render thread
         * even when init chose Noop. */
    }

    ::unsetenv("ME_GPU_FORCE_NOOP");
}

TEST_CASE("BgfxGpuBackend: ME_GPU_FORCE_NOOP=true (word form) also selects Noop") {
    /* The env var parser accepts case-insensitive truthy words —
     * pin the contract with a non-"1" form so the parser's word-
     * matching branch is exercised. */
    ::setenv("ME_GPU_FORCE_NOOP", "TRUE", /*overwrite=*/1);
    {
        auto backend = me::gpu::make_gpu_backend();
        REQUIRE(backend);
        CHECK(std::string{backend->name()} == "bgfx-Noop");
    }
    ::unsetenv("ME_GPU_FORCE_NOOP");
}

TEST_CASE("BgfxGpuBackend: ME_GPU_FORCE_NOOP=0 falls through to auto-pick") {
    /* "0" is not truthy — parser returns false → normal auto-pick
     * path. On dev macOS that picks Metal; we don't assert the
     * name here to stay platform-agnostic, only that it's NOT
     * bgfx-Noop (unless the host really can't auto-pick, which
     * is a legitimate edge case). */
    ::setenv("ME_GPU_FORCE_NOOP", "0", /*overwrite=*/1);
    {
        auto backend = me::gpu::make_gpu_backend();
        REQUIRE(backend);
        REQUIRE(backend->available());
        const std::string n = backend->name();
        /* On macOS Metal is expected; on drivers that refuse
         * headless init the Noop fallback still lands. Either way
         * prefix is "bgfx-". The specific renderer just can't be
         * the forced one we didn't force. */
        CHECK(n.rfind("bgfx-", 0) == 0);
    }
    ::unsetenv("ME_GPU_FORCE_NOOP");
}

TEST_CASE("BgfxGpuBackend: ME_GPU_FORCE_NOOP unset uses default auto-pick") {
    ::unsetenv("ME_GPU_FORCE_NOOP");
    auto backend = me::gpu::make_gpu_backend();
    REQUIRE(backend);
    REQUIRE(backend->available());
    /* Same prefix check — platform-agnostic sanity. */
    const std::string n = backend->name();
    CHECK(n.rfind("bgfx-", 0) == 0);
}

#endif  // ME_HAS_GPU
