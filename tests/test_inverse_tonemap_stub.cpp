/*
 * test_inverse_tonemap_stub — pins the registered-but-deferred
 * contract for `me::compose::apply_inverse_tonemap_inplace`.
 *
 * The kernel is a stub today (M10 exit criterion 6 second half;
 * impl tracked under `inverse-tonemap-effect-impl` in P2). What
 * this suite asserts:
 *   - Argument-shape rejects land before the UNSUPPORTED short-
 *     circuit (so a future impl can drop in without rewriting
 *     the prologue).
 *   - Otherwise-valid input returns ME_E_UNSUPPORTED — the
 *     deterministic stub answer.
 *   - The buffer is NOT mutated by the stub (early-return before
 *     any pixel touch).
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

TEST_CASE("inverse_tonemap stub: null buffer rejected with INVALID_ARG") {
    me::InverseTonemapEffectParams p{};
    CHECK(me::compose::apply_inverse_tonemap_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap stub: non-positive dimensions rejected") {
    auto buf = make_known(16, 16);
    me::InverseTonemapEffectParams p{};
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, -1, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap stub: stride < width*4 rejected") {
    auto buf = make_known(16, 16);
    me::InverseTonemapEffectParams p{};
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, 16, 32, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap stub: target_peak_nits <= 0 rejected") {
    auto buf = make_known(16, 16);
    me::InverseTonemapEffectParams p{};
    p.target_peak_nits = 0.0;
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.target_peak_nits = -100.0;
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("inverse_tonemap stub: valid input returns ME_E_UNSUPPORTED") {
    /* Argument-validation passes → stub returns the documented
     * UNSUPPORTED. Run on both algos to pin that the dispatch
     * surface is registered for both. */
    auto buf = make_known(8, 8);
    me::InverseTonemapEffectParams p_lin{}, p_hable{};
    p_lin.algo   = me::InverseTonemapEffectParams::Algo::Linear;
    p_hable.algo = me::InverseTonemapEffectParams::Algo::Hable;

    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 8, 8, 32, p_lin)
          == ME_E_UNSUPPORTED);
    CHECK(me::compose::apply_inverse_tonemap_inplace(buf.data(), 8, 8, 32, p_hable)
          == ME_E_UNSUPPORTED);
}

TEST_CASE("inverse_tonemap stub: buffer is NOT mutated") {
    /* Stub must early-return before touching pixels — when the
     * impl lands, this guard fails first if the impl forgets to
     * gate behind a feature flag. */
    auto buf = make_known(8, 8);
    const auto snapshot = buf;
    me::InverseTonemapEffectParams p{};
    REQUIRE(me::compose::apply_inverse_tonemap_inplace(buf.data(), 8, 8, 32, p)
            == ME_E_UNSUPPORTED);
    CHECK(buf == snapshot);
}
