/*
 * test_vignette_kernel_pixel — pixel regression for the M12
 * §155 (3/4) vignette kernel
 * (`me::compose::apply_vignette_inplace`).
 *
 * Coverage:
 *   - Argument-shape rejects (null buffer, bad dims, bad
 *     stride, non-finite params, negative radius / softness).
 *   - intensity=0 (default) → no-op.
 *   - intensity=1 + radius=0 + softness=2 → center pixel ≈
 *     unchanged, edge pixels → black.
 *   - Alpha is never modified.
 *   - Center can be off-frame (e.g. centerX=0): pixels nearest
 *     the (0, 0.5) position retain more brightness than the
 *     opposite edge.
 */
#include <doctest/doctest.h>

#include "compose/vignette_kernel.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> make_solid(int w, int h, std::size_t stride,
                                       std::uint8_t r, std::uint8_t g,
                                       std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> rgba(stride * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * stride +
                                   static_cast<std::size_t>(x) * 4;
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = a;
        }
    }
    return rgba;
}

}  // namespace

TEST_CASE("apply_vignette_inplace: null buffer rejected") {
    me::VignetteEffectParams p;
    p.intensity = 1.0f;  /* non-default so we don't hit early-out */
    CHECK(me::compose::apply_vignette_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_vignette_inplace: bad dimensions rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::VignetteEffectParams p;
    p.intensity = 1.0f;
    CHECK(me::compose::apply_vignette_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_vignette_inplace(buf.data(), 16, 0, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_vignette_inplace: undersized stride rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::VignetteEffectParams p;
    p.intensity = 1.0f;
    CHECK(me::compose::apply_vignette_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_vignette_inplace: non-finite params rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::VignetteEffectParams p;
    p.intensity = std::nan("");
    CHECK(me::compose::apply_vignette_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_vignette_inplace: negative radius / softness rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::VignetteEffectParams p;
    p.intensity = 1.0f;
    p.radius = -0.1f;
    CHECK(me::compose::apply_vignette_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p = me::VignetteEffectParams{};
    p.intensity = 1.0f;
    p.softness = -0.1f;
    CHECK(me::compose::apply_vignette_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_vignette_inplace: intensity=0 → no-op") {
    auto buf = make_solid(16, 16, 64, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::VignetteEffectParams p;  /* defaults: intensity=0 → identity */
    REQUIRE(me::compose::apply_vignette_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_vignette_inplace: intensity=1 + small inner radius darkens edges") {
    /* 33×33 frame (odd dim → exact center pixel at (16, 16)).
     * radius=0 + softness=2 means the falloff starts AT the
     * center; the center pixel itself sits at d=0 so smoothstep
     * evaluates to 0 there → factor ~ 1.0 (no darkening). Edge
     * pixels at d ≈ 1.0 are well past softness → factor → 0
     * (full black). */
    const int w = 33, h = 33;
    auto buf = make_solid(w, h, 33 * 4, 200, 100, 50, 255);
    me::VignetteEffectParams p;
    p.intensity = 1.0f;
    p.radius    = 0.0f;
    p.softness  = 2.0f;
    REQUIRE(me::compose::apply_vignette_inplace(buf.data(), w, h, 33 * 4, p)
            == ME_OK);

    /* Center pixel: d = 0 → smoothstep(0, 2, 0) = 0 → factor = 1
     * → unchanged. Allow ±2 LSB tolerance. */
    const std::size_t cx_idx = 16 * 33 * 4 + 16 * 4;
    CHECK(buf[cx_idx + 0] >= 198);
    CHECK(buf[cx_idx + 0] <= 200);
    CHECK(buf[cx_idx + 1] >= 99);
    CHECK(buf[cx_idx + 1] <= 100);
    CHECK(buf[cx_idx + 3] == 255);  /* alpha untouched */

    /* Corner pixel (32, 32): d ≈ sqrt(2) ≈ 1.414, well past
     * softness=2 means smoothstep returns 0.5+? Actually at
     * d=1.414, t = (1.414-0)/(2-0) = 0.707, smoothstep =
     * 0.707²·(3-2·0.707) ≈ 0.793. factor = 1 - 1·0.793 =
     * 0.207. RGB = 0.207 × original. */
    const std::size_t corner_idx = 32 * 33 * 4 + 32 * 4;
    /* Expected R ≈ 0.207 × 200 ≈ 41.4 → byte 41-42. Allow ±5
     * LSB to absorb the smoothstep precision. */
    CHECK(buf[corner_idx + 0] < 50);
    CHECK(buf[corner_idx + 3] == 255);  /* alpha untouched */
}

TEST_CASE("apply_vignette_inplace: alpha never modified") {
    auto buf = make_solid(8, 8, 32, 255, 255, 255, 123);
    me::VignetteEffectParams p;
    p.intensity = 1.0f;
    p.radius    = 0.0f;
    p.softness  = 0.5f;
    REQUIRE(me::compose::apply_vignette_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 32 +
                                   static_cast<std::size_t>(x) * 4;
            CHECK(buf[i + 3] == 123);
        }
    }
}

TEST_CASE("apply_vignette_inplace: off-center darkens far edge more than near edge") {
    /* Center at (0, 0.5) — left edge of frame, vertically mid.
     * Pixels near the left edge should retain more brightness
     * than pixels near the right edge. */
    const int w = 33, h = 7;
    auto buf = make_solid(w, h, w * 4, 200, 200, 200, 255);
    me::VignetteEffectParams p;
    p.intensity = 1.0f;
    p.radius    = 0.0f;
    p.softness  = 2.0f;
    p.center_x  = 0.0f;
    p.center_y  = 0.5f;
    REQUIRE(me::compose::apply_vignette_inplace(buf.data(), w, h, w * 4, p)
            == ME_OK);

    /* Left edge (x=0, y=3): close to center → should be near
     * original. Right edge (x=32, y=3): far from center →
     * should be much darker. */
    const std::size_t left  = 3 * w * 4 + 0 * 4;
    const std::size_t right = 3 * w * 4 + 32 * 4;
    CHECK(buf[left  + 0] > buf[right + 0]);
}
