/*
 * test_tonemap_kernel — pixel + determinism coverage for
 * `me::compose::apply_tonemap_inplace`. M10 exit criterion 6
 * first half (`tonemap-effect-hable`).
 *
 * What this suite pins:
 *   - Hable / Reinhard / ACES each produce deterministic byte
 *     output for a fixed input + target_nits.
 *   - Highlights at target_nits > 100 are rolled off (output <
 *     input) — exercises the roll-off path on bright pixels.
 *   - Black pixels stay near black across all three algos.
 *   - Alpha pass-through (the kernel must not touch px[3]).
 *   - Repeated apply on the same buffer is byte-identical
 *     (deterministic software path per VISION §3.1).
 *   - Argument-validation rejections (null buffer, bad dims,
 *     bad stride, target_nits ≤ 0).
 */
#include <doctest/doctest.h>

#include "compose/tonemap_kernel.hpp"

#include <cstdint>
#include <vector>

namespace {

/* Construct a deterministic RGBA8 gradient. Pixel (x, y) =
 * { (x*4) & 0xff, (y*4) & 0xff, ((x+y)*2) & 0xff, 0x80 }. Same
 * function across the suite so the byte-identical determinism
 * tests have a known input. */
std::vector<std::uint8_t> make_gradient(int w, int h) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* px = buf.data() + (y * w + x) * 4;
            px[0] = static_cast<std::uint8_t>((x * 4) & 0xff);
            px[1] = static_cast<std::uint8_t>((y * 4) & 0xff);
            px[2] = static_cast<std::uint8_t>(((x + y) * 2) & 0xff);
            px[3] = 0x80;
        }
    }
    return buf;
}

}  // namespace

TEST_CASE("apply_tonemap_inplace: rejects null buffer") {
    me::TonemapEffectParams p{};
    CHECK(me::compose::apply_tonemap_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tonemap_inplace: rejects non-positive dimensions") {
    auto buf = make_gradient(16, 16);
    me::TonemapEffectParams p{};
    CHECK(me::compose::apply_tonemap_inplace(buf.data(), 0, 16, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_tonemap_inplace(buf.data(), 16, 0, 64, p)
          == ME_E_INVALID_ARG);
    CHECK(me::compose::apply_tonemap_inplace(buf.data(), -1, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tonemap_inplace: rejects stride < width*4") {
    auto buf = make_gradient(16, 16);
    me::TonemapEffectParams p{};
    /* width*4 = 64; passing stride=63 is bogus. */
    CHECK(me::compose::apply_tonemap_inplace(buf.data(), 16, 16, 63, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tonemap_inplace: rejects target_nits <= 0") {
    auto buf = make_gradient(16, 16);
    me::TonemapEffectParams p{};
    p.target_nits = 0.0;
    CHECK(me::compose::apply_tonemap_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
    p.target_nits = -100.0;
    CHECK(me::compose::apply_tonemap_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_tonemap_inplace: Hable preserves alpha untouched") {
    auto buf = make_gradient(16, 16);
    /* Snapshot alphas before. The gradient sets all alphas to 0x80;
     * the kernel must leave them at 0x80. */
    me::TonemapEffectParams p{};
    p.algo = me::TonemapEffectParams::Algo::Hable;
    REQUIRE(me::compose::apply_tonemap_inplace(buf.data(), 16, 16, 64, p)
            == ME_OK);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            CHECK(buf[(y * 16 + x) * 4 + 3] == 0x80);
        }
    }
}

TEST_CASE("apply_tonemap_inplace: black pixel stays near black (all algos)") {
    me::TonemapEffectParams p{};
    for (auto algo : {me::TonemapEffectParams::Algo::Hable,
                       me::TonemapEffectParams::Algo::Reinhard,
                       me::TonemapEffectParams::Algo::ACES}) {
        p.algo = algo;
        std::vector<std::uint8_t> buf{0, 0, 0, 0xff};
        REQUIRE(me::compose::apply_tonemap_inplace(buf.data(), 1, 1, 4, p)
                == ME_OK);
        /* All three curves map 0 → 0 (or nearly so — ACES has a
         * tiny `b/e` shoulder lift). Allow ≤ 10/255 latitude. */
        CHECK(static_cast<int>(buf[0]) <= 10);
        CHECK(static_cast<int>(buf[1]) <= 10);
        CHECK(static_cast<int>(buf[2]) <= 10);
    }
}

TEST_CASE("apply_tonemap_inplace: highlight roll-off at target_nits=400") {
    /* target_nits=400 means input byte 255 maps to linear 4.0
     * (400/100) — well into the roll-off region. Hable / Reinhard
     * both compress this back to the byte range; the OUTPUT byte
     * should be < 255 because the curves never reach saturation
     * for finite input. ACES additionally clamps internally. */
    me::TonemapEffectParams p{};
    p.target_nits = 400.0;

    for (auto algo : {me::TonemapEffectParams::Algo::Hable,
                       me::TonemapEffectParams::Algo::Reinhard,
                       me::TonemapEffectParams::Algo::ACES}) {
        p.algo = algo;
        /* All-white input. */
        std::vector<std::uint8_t> buf{255, 255, 255, 255};
        REQUIRE(me::compose::apply_tonemap_inplace(buf.data(), 1, 1, 4, p)
                == ME_OK);
        /* Hable + Reinhard roll off well below 255 at linear=4.0;
         * ACES clamps to 1.0 (byte 255). The contract: output ≤
         * input for the highlight case (no curve ever brightens). */
        CHECK(static_cast<int>(buf[0]) <= 255);
        if (algo != me::TonemapEffectParams::Algo::ACES) {
            CHECK(static_cast<int>(buf[0]) < 255);
        }
    }
}

TEST_CASE("apply_tonemap_inplace: byte-deterministic across repeated calls") {
    /* Same input → same output, every algo. Run the kernel twice on
     * fresh copies of the same gradient and CHECK byte-identity.
     * Guards against future SIMD-dispatch / parallelism creep. */
    me::TonemapEffectParams p{};
    p.target_nits = 200.0;

    for (auto algo : {me::TonemapEffectParams::Algo::Hable,
                       me::TonemapEffectParams::Algo::Reinhard,
                       me::TonemapEffectParams::Algo::ACES}) {
        p.algo = algo;
        auto a = make_gradient(32, 32);
        auto b = make_gradient(32, 32);
        REQUIRE(me::compose::apply_tonemap_inplace(a.data(), 32, 32, 128, p)
                == ME_OK);
        REQUIRE(me::compose::apply_tonemap_inplace(b.data(), 32, 32, 128, p)
                == ME_OK);
        CHECK(a == b);
    }
}

TEST_CASE("apply_tonemap_inplace: distinct algos produce distinct outputs") {
    /* Hable vs Reinhard vs ACES on the same input must differ —
     * if the dispatch ever collapses (e.g. wrong template
     * specialisation), this catches it. */
    me::TonemapEffectParams p_h{}, p_r{}, p_a{};
    p_h.algo = me::TonemapEffectParams::Algo::Hable;
    p_r.algo = me::TonemapEffectParams::Algo::Reinhard;
    p_a.algo = me::TonemapEffectParams::Algo::ACES;
    p_h.target_nits = p_r.target_nits = p_a.target_nits = 200.0;

    auto h = make_gradient(8, 8);
    auto r = make_gradient(8, 8);
    auto ac = make_gradient(8, 8);
    REQUIRE(me::compose::apply_tonemap_inplace(h.data(),  8, 8, 32, p_h) == ME_OK);
    REQUIRE(me::compose::apply_tonemap_inplace(r.data(),  8, 8, 32, p_r) == ME_OK);
    REQUIRE(me::compose::apply_tonemap_inplace(ac.data(), 8, 8, 32, p_a) == ME_OK);

    CHECK(h  != r);
    CHECK(h  != ac);
    CHECK(r  != ac);
}
