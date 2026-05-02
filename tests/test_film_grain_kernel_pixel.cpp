/*
 * test_film_grain_kernel_pixel — pixel regression for the M12
 * §155 (4/4) film-grain kernel
 * (`me::compose::apply_film_grain_inplace`).
 *
 * Coverage:
 *   - Argument-shape rejects (null buffer, bad dims, bad
 *     stride, non-finite amount, out-of-range grain_size).
 *   - amount=0 (default) → no-op.
 *   - Determinism: same seed + same input bytes → same output
 *     bytes across two independent invocations.
 *   - Different seeds → different output bytes (high
 *     probability; checked over a 16x16 patch).
 *   - Alpha is never modified.
 *   - grain_size_px > 1 produces block-shaped patches
 *     (adjacent pixels in the same block share the same
 *     delta).
 */
#include <doctest/doctest.h>

#include "compose/film_grain_kernel.hpp"

#include <algorithm>
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

TEST_CASE("apply_film_grain_inplace: null buffer rejected") {
    me::FilmGrainEffectParams p;
    p.seed = 42; p.amount = 0.5f;
    CHECK(me::compose::apply_film_grain_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_film_grain_inplace: bad dimensions rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::FilmGrainEffectParams p;
    p.seed = 42; p.amount = 0.5f;
    CHECK(me::compose::apply_film_grain_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_film_grain_inplace: undersized stride rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::FilmGrainEffectParams p;
    p.seed = 42; p.amount = 0.5f;
    CHECK(me::compose::apply_film_grain_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_film_grain_inplace: non-finite amount rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::FilmGrainEffectParams p;
    p.amount = std::nan("");
    CHECK(me::compose::apply_film_grain_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_film_grain_inplace: out-of-range grain_size_px rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::FilmGrainEffectParams p;
    p.amount = 0.5f;
    p.grain_size_px = 0;
    CHECK(me::compose::apply_film_grain_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.grain_size_px = 9;
    CHECK(me::compose::apply_film_grain_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_film_grain_inplace: amount=0 → no-op") {
    auto buf = make_solid(16, 16, 64, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::FilmGrainEffectParams p;  /* defaults: amount=0 */
    REQUIRE(me::compose::apply_film_grain_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_film_grain_inplace: same seed + same input → same output (determinism)") {
    auto a = make_solid(16, 16, 64, 128, 128, 128, 255);
    auto b = make_solid(16, 16, 64, 128, 128, 128, 255);
    me::FilmGrainEffectParams p;
    p.seed = 0xABCD'EF12'3456'7890ULL;
    p.amount = 0.5f;
    REQUIRE(me::compose::apply_film_grain_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_film_grain_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    /* Byte-identical across the two invocations. VISION §3.1
     * byte-identity verification. */
    CHECK(a == b);
}

TEST_CASE("apply_film_grain_inplace: different seeds → different outputs") {
    auto a = make_solid(16, 16, 64, 128, 128, 128, 255);
    auto b = make_solid(16, 16, 64, 128, 128, 128, 255);
    me::FilmGrainEffectParams pa, pb;
    pa.seed = 0x1111;
    pb.seed = 0x2222;
    pa.amount = pb.amount = 0.5f;
    REQUIRE(me::compose::apply_film_grain_inplace(a.data(), 16, 16, 64, pa)
            == ME_OK);
    REQUIRE(me::compose::apply_film_grain_inplace(b.data(), 16, 16, 64, pb)
            == ME_OK);
    /* xorshift64 + FNV-1a mixing produces different bytes for
     * different seeds (collision probability vanishingly low
     * for a 16×16 grid). */
    CHECK(a != b);
}

TEST_CASE("apply_film_grain_inplace: alpha never modified") {
    auto buf = make_solid(8, 8, 32, 100, 200, 50, 77);
    me::FilmGrainEffectParams p;
    p.seed = 999; p.amount = 1.0f;  /* full-range noise */
    REQUIRE(me::compose::apply_film_grain_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 32 +
                                   static_cast<std::size_t>(x) * 4;
            CHECK(buf[i + 3] == 77);
        }
    }
}

TEST_CASE("apply_film_grain_inplace: grain_size_px=4 → 4x4 blocks share the same delta") {
    auto buf = make_solid(16, 16, 64, 128, 128, 128, 255);
    me::FilmGrainEffectParams p;
    p.seed = 0x42; p.amount = 1.0f;
    p.grain_size_px = 4;
    REQUIRE(me::compose::apply_film_grain_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);

    /* Every 4×4 block should have identical RGB values
     * (because the noise is keyed on (block_x, block_y), not
     * per-pixel). Verify the (0,0) block — pixels (0,0),
     * (1,0), (0,1), (3,3) all share the same delta. */
    auto pix = [&](int x, int y) {
        const std::size_t i = static_cast<std::size_t>(y) * 64 +
                               static_cast<std::size_t>(x) * 4;
        return std::array<std::uint8_t, 3>{buf[i], buf[i+1], buf[i+2]};
    };
    const auto p00 = pix(0, 0);
    CHECK(pix(1, 0) == p00);
    CHECK(pix(0, 1) == p00);
    CHECK(pix(3, 3) == p00);
    /* Adjacent block (4, 0) likely differs from (0, 0). */
    /* Note: collisions possible but vanishingly rare with FNV
     * mixing — we don't strictly assert difference, only that
     * within-block consistency holds. */
}
