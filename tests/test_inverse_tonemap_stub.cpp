/*
 * test_inverse_tonemap_stub — pins the partial-impl contract for
 * `me::compose::apply_inverse_tonemap_inplace`.
 *
 * Cycle 24 landed `Linear`; `Hable` remains UNSUPPORTED (tracked as
 * `inverse-tonemap-hable-impl`). What this suite asserts:
 *
 *   - Argument-shape rejects land before the algo dispatch (so a
 *     future Hable impl drops in without rewriting the prologue).
 *   - Hable still returns ME_E_UNSUPPORTED with the buffer NOT
 *     mutated (early-return before any pixel touch).
 *   - Linear at target_peak_nits=100 is exact identity (multiply
 *     by 1.0).
 *   - Linear at target_peak_nits=200 doubles inputs, clipping at
 *     128+ (because 128 × 2 = 256 → byte 255).
 *   - Linear at target_peak_nits=1000 (HDR10 default) clips above
 *     byte 26 to 255 — the documented "loses HDR range" placeholder
 *     behaviour.
 *
 * The file kept its `_stub` slug for git-history continuity even
 * though the kernel is no longer purely a stub; the name remains
 * accurate for the Hable algo half.
 */
#include <doctest/doctest.h>

#include "compose/inverse_tonemap_kernel.hpp"

#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> make_known(int w, int h) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::uint8_t>(i & 0xff);
    }
    return buf;
}

}  // namespace

TEST_CASE("inverse_tonemap: null buffer rejected with INVALID_ARG") {
    me::InverseTonemapEffectParams p{};
    CHECK(me::compose::apply_inverse_tonemap_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap: non-positive dimensions rejected") {
    auto buf = make_known(16, 16);
    me::InverseTonemapEffectParams p{};
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, -1, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap: stride < width*4 rejected") {
    auto buf = make_known(16, 16);
    me::InverseTonemapEffectParams p{};
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap: target_peak_nits <= 0 rejected") {
    auto buf = make_known(16, 16);
    me::InverseTonemapEffectParams p{};
    p.target_peak_nits = 0.0;
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.target_peak_nits = -100.0;
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap Hable: still UNSUPPORTED, buffer unchanged") {
    auto buf = make_known(8, 8);
    const auto snapshot = buf;
    me::InverseTonemapEffectParams p{};
    p.algo = me::InverseTonemapEffectParams::Algo::Hable;
    REQUIRE(me::compose::apply_inverse_tonemap_inplace(buf.data(), 8, 8, 32, p)
            == ME_E_UNSUPPORTED);
    CHECK(buf == snapshot);
}

TEST_CASE("inverse_tonemap Linear @ 100 nits: identity") {
    /* target_peak_nits=100 → headroom=1.0 → out = clamp(in*1, 0, 255) = in.
     * Pin for every byte position 0..255 exactly. */
    auto buf = make_known(16, 16);
    const auto snapshot = buf;
    me::InverseTonemapEffectParams p{};
    p.algo             = me::InverseTonemapEffectParams::Algo::Linear;
    p.target_peak_nits = 100.0;
    REQUIRE(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("inverse_tonemap Linear @ 200 nits: ×2 with byte clipping") {
    /* target_peak_nits=200 → headroom=2.0 → out = clamp(in*2, 0, 255).
     * Synthetic 4×1 frame with known RGBA values: (10, 100, 127, 255). */
    std::vector<std::uint8_t> buf{
        10,  100, 127, 255,   /* px 0: B=255 alpha — should pass through */
        50,  128, 200,  64,   /* px 1: alpha pass-through */
        0,   255,   1,   8,   /* px 2 */
        200,   0, 100, 200,   /* px 3 */
    };
    me::InverseTonemapEffectParams p{};
    p.algo             = me::InverseTonemapEffectParams::Algo::Linear;
    p.target_peak_nits = 200.0;
    REQUIRE(me::compose::apply_inverse_tonemap_inplace(buf.data(), 4, 1, 16, p)
            == ME_OK);

    /* Doubled where < 128, clamped to 255 above; alpha untouched. */
    CHECK(buf[0]  == 20);   CHECK(buf[1]  == 200); CHECK(buf[2]  == 254);
    CHECK(buf[3]  == 255);  /* alpha kept */
    CHECK(buf[4]  == 100);  CHECK(buf[5]  == 255); CHECK(buf[6]  == 255);
    CHECK(buf[7]  == 64);   /* alpha kept */
    CHECK(buf[8]  == 0);    CHECK(buf[9]  == 255); CHECK(buf[10] == 2);
    CHECK(buf[11] == 8);    /* alpha kept */
    CHECK(buf[12] == 255);  CHECK(buf[13] == 0);   CHECK(buf[14] == 200);
    CHECK(buf[15] == 200);  /* alpha kept */
}

TEST_CASE("inverse_tonemap Linear @ 1000 nits: clips above byte 25") {
    /* target_peak_nits=1000 (HDR10 default) → headroom=10. byte 25 →
     * 250, byte 26 → 260 → 255. Documented placeholder behaviour. */
    std::vector<std::uint8_t> buf{
        25,  26,  100, 200,   /* alpha 200 should pass through */
    };
    me::InverseTonemapEffectParams p{};
    p.algo             = me::InverseTonemapEffectParams::Algo::Linear;
    p.target_peak_nits = 1000.0;
    REQUIRE(me::compose::apply_inverse_tonemap_inplace(buf.data(), 1, 1, 4, p)
            == ME_OK);
    CHECK(buf[0] == 250);   /* 25 × 10 = 250 */
    CHECK(buf[1] == 255);   /* 26 × 10 = 260 → clamp 255 */
    CHECK(buf[2] == 255);   /* clamps */
    CHECK(buf[3] == 200);   /* alpha pass-through */
}

TEST_CASE("inverse_tonemap Linear: deterministic across runs") {
    /* Determinism guard for VISION §3.1: same input bytes + params →
     * same output bytes across two passes through fresh kernels. */
    auto buf_a = make_known(32, 32);
    auto buf_b = make_known(32, 32);
    me::InverseTonemapEffectParams p{};
    p.algo             = me::InverseTonemapEffectParams::Algo::Linear;
    p.target_peak_nits = 800.0;
    REQUIRE(me::compose::apply_inverse_tonemap_inplace(buf_a.data(), 32, 32, 128, p)
            == ME_OK);
    REQUIRE(me::compose::apply_inverse_tonemap_inplace(buf_b.data(), 32, 32, 128, p)
            == ME_OK);
    CHECK(buf_a == buf_b);
}
