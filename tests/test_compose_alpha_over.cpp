/*
 * test_compose_alpha_over — numerical tripwire for me::compose::alpha_over.
 *
 * The kernel is pure math (RGBA8 + opacity + BlendMode → RGBA8), so the
 * test suite pins the contract with known-good input/output triples for
 * each of the 3 blend modes plus the opacity scaling behavior and
 * determinism guarantee. No host env / fixture needed — these all run
 * against in-memory 1×1 or small buffers.
 *
 * Tolerance: uint8 outputs are compared exactly. IEEE-754 float32 with
 * round-to-nearest (`lroundf`) is deterministic across macOS / Linux /
 * Windows without -ffast-math / FMA, so any drift from the expected
 * values is a real regression.
 */
#include <doctest/doctest.h>

#include "compose/alpha_over.hpp"

#include <array>
#include <cstdint>
#include <vector>

using me::compose::alpha_over;
using me::compose::BlendMode;

namespace {

/* Build a 1×1 RGBA8 buffer shorthand. */
std::array<uint8_t, 4> px(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return {r, g, b, a};
}

void composite_1x1(std::array<uint8_t, 4>&       dst,
                    const std::array<uint8_t, 4>& src,
                    float                         opacity,
                    BlendMode                     mode) {
    alpha_over(dst.data(), src.data(),
               /*width=*/1, /*height=*/1, /*stride_bytes=*/4,
               opacity, mode);
}

}  // namespace

TEST_CASE("alpha_over normal: fully opaque src replaces dst") {
    auto dst = px(0,   0,   0,   255);  /* opaque black */
    auto src = px(255, 128, 64,  255);  /* opaque orange */
    composite_1x1(dst, src, 1.0f, BlendMode::Normal);
    CHECK(dst[0] == 255);
    CHECK(dst[1] == 128);
    CHECK(dst[2] == 64);
    CHECK(dst[3] == 255);
}

TEST_CASE("alpha_over normal: fully transparent src leaves dst unchanged") {
    auto dst = px(10, 20, 30, 255);
    auto src = px(255, 0, 0, 0);   /* src alpha 0 */
    composite_1x1(dst, src, 1.0f, BlendMode::Normal);
    CHECK(dst[0] == 10);
    CHECK(dst[1] == 20);
    CHECK(dst[2] == 30);
    CHECK(dst[3] == 255);
}

TEST_CASE("alpha_over normal: 50% src alpha produces 50/50 mix (opaque dst)") {
    auto dst = px(0,   0,   0,   255);  /* opaque black */
    auto src = px(200, 200, 200, 128);  /* 50.2% alpha light gray */
    composite_1x1(dst, src, 1.0f, BlendMode::Normal);
    /* 200 * (128/255) + 0 * (1 - 128/255) = 200 * 0.50196 = 100.39 → 100 */
    CHECK(dst[0] == 100);
    CHECK(dst[1] == 100);
    CHECK(dst[2] == 100);
    /* alpha_out = 128/255 + 255/255 * (1 - 128/255) = 255 */
    CHECK(dst[3] == 255);
}

TEST_CASE("alpha_over normal: opacity parameter scales src alpha") {
    auto dst = px(0, 0, 0, 255);
    auto src = px(255, 0, 0, 255);  /* fully opaque red */
    composite_1x1(dst, src, 0.5f, BlendMode::Normal);
    /* 0.5 opacity on alpha=255 → effective alpha ~128/255 */
    /* 255 * 0.5 + 0 * 0.5 = 127.5 → 128 */
    CHECK(dst[0] == 128);
    CHECK(dst[1] == 0);
    CHECK(dst[2] == 0);
}

TEST_CASE("alpha_over normal: opacity 0 is a no-op on dst") {
    auto dst = px(42, 43, 44, 255);
    auto src = px(255, 255, 255, 255);
    composite_1x1(dst, src, 0.0f, BlendMode::Normal);
    CHECK(dst[0] == 42);
    CHECK(dst[1] == 43);
    CHECK(dst[2] == 44);
}

TEST_CASE("alpha_over multiply: white src preserves dst RGB") {
    auto dst = px(100, 120, 140, 255);
    auto src = px(255, 255, 255, 255);   /* white, fully opaque */
    composite_1x1(dst, src, 1.0f, BlendMode::Multiply);
    /* src_blended = src * dst = dst; src-over with src_a=1 → dst */
    CHECK(dst[0] == 100);
    CHECK(dst[1] == 120);
    CHECK(dst[2] == 140);
}

TEST_CASE("alpha_over multiply: black src produces black when src_a=1") {
    auto dst = px(200, 200, 200, 255);
    auto src = px(0, 0, 0, 255);   /* black, fully opaque */
    composite_1x1(dst, src, 1.0f, BlendMode::Multiply);
    /* src_blended = 0 * dst = 0; src-over with src_a=1 → 0 */
    CHECK(dst[0] == 0);
    CHECK(dst[1] == 0);
    CHECK(dst[2] == 0);
}

TEST_CASE("alpha_over screen: black src preserves dst") {
    auto dst = px(100, 120, 140, 255);
    auto src = px(0, 0, 0, 255);   /* black, fully opaque */
    composite_1x1(dst, src, 1.0f, BlendMode::Screen);
    /* src_blended = 1 - (1-0)*(1-dst) = dst; src-over with src_a=1 → dst */
    CHECK(dst[0] == 100);
    CHECK(dst[1] == 120);
    CHECK(dst[2] == 140);
}

TEST_CASE("alpha_over screen: white src produces white when src_a=1") {
    auto dst = px(50, 50, 50, 255);
    auto src = px(255, 255, 255, 255);   /* white, fully opaque */
    composite_1x1(dst, src, 1.0f, BlendMode::Screen);
    /* src_blended = 1 - (1-1)*(1-dst) = 1 (white); src-over → white */
    CHECK(dst[0] == 255);
    CHECK(dst[1] == 255);
    CHECK(dst[2] == 255);
}

TEST_CASE("alpha_over: same inputs produce byte-identical outputs (determinism)") {
    /* Multi-pixel buffer across blend modes; regression guard for
     * float-math drift, ordering, or SIMD creep. */
    std::vector<uint8_t> base(64, 0);
    for (size_t i = 0; i < base.size(); ++i) base[i] = static_cast<uint8_t>(i * 3 + 1);

    auto run = [&](BlendMode mode) {
        std::vector<uint8_t> dst(64, 0);
        for (size_t i = 0; i < dst.size(); ++i) dst[i] = static_cast<uint8_t>(i * 7 + 2);
        alpha_over(dst.data(), base.data(), /*w=*/4, /*h=*/4, /*stride=*/16, 0.75f, mode);
        return dst;
    };

    const auto a1 = run(BlendMode::Normal);
    const auto a2 = run(BlendMode::Normal);
    CHECK(a1 == a2);

    const auto m1 = run(BlendMode::Multiply);
    const auto m2 = run(BlendMode::Multiply);
    CHECK(m1 == m2);

    const auto s1 = run(BlendMode::Screen);
    const auto s2 = run(BlendMode::Screen);
    CHECK(s1 == s2);

    /* Sanity: different blend modes should produce different results on
     * this non-trivial input — otherwise blend_mode_apply is broken. */
    CHECK(a1 != m1);
    CHECK(a1 != s1);
    CHECK(m1 != s1);
}

TEST_CASE("alpha_over: zero-size buffer is a no-op") {
    /* Guard: compose kernel on 0×H / W×0 should not touch memory. We
     * can't easily assert no memory access, but at least it must not
     * crash or loop infinitely. */
    uint8_t nothing = 0;
    alpha_over(&nothing, &nothing, /*w=*/0, /*h=*/100, /*stride=*/0, 1.0f, BlendMode::Normal);
    alpha_over(&nothing, &nothing, /*w=*/100, /*h=*/0, /*stride=*/400, 1.0f, BlendMode::Normal);
    CHECK(nothing == 0);
}
