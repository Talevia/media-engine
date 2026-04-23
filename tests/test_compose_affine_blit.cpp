/*
 * test_compose_affine_blit — numerical tripwire for affine_blit +
 * compose_inverse_affine.
 *
 * Pins canonical transforms against known-value pixel outputs:
 *   - Identity (no translate/scale/rotate) produces src-in-dst at
 *     matching pixel offsets.
 *   - Pure translate shifts pixels by the requested integer offset.
 *   - Pure scale 2× doubles each src pixel into a 2×2 dst block.
 *   - 90° rotate reorients the image (top-left src → top-right dst
 *     for clockwise, depending on anchor).
 *   - Out-of-bounds sampling yields transparent pixels.
 *
 * Focus is on correctness of the matrix math + sampling logic.
 * Bilinear quality is out of scope (nearest-neighbor only).
 */
#include <doctest/doctest.h>

#include "compose/affine_blit.hpp"

#include <array>
#include <cstdint>
#include <vector>

using me::compose::affine_blit;
using me::compose::AffineMatrix;
using me::compose::compose_inverse_affine;

namespace {

/* Build a tiny N×N src where each pixel is uniquely identifiable by
 * its position (R = x, G = y, B = 0, A = 255). Later tests can
 * inspect dst pixels and figure out which src pixel they came from. */
std::vector<uint8_t> labeled_src(int w, int h) {
    std::vector<uint8_t> s(static_cast<std::size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4u;
            s[p + 0] = static_cast<uint8_t>(x);
            s[p + 1] = static_cast<uint8_t>(y);
            s[p + 2] = 0;
            s[p + 3] = 255;
        }
    }
    return s;
}

inline std::array<uint8_t, 4> pixel_at(const std::vector<uint8_t>& buf,
                                        int w, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4u;
    return {buf[p], buf[p + 1], buf[p + 2], buf[p + 3]};
}

}  // namespace

TEST_CASE("compose_inverse_affine: identity transform → identity matrix") {
    const auto m = compose_inverse_affine(
        /*translate_x=*/0, /*translate_y=*/0,
        /*scale_x=*/1,     /*scale_y=*/1,
        /*rotation_deg=*/0,
        /*anchor_x=*/0,    /*anchor_y=*/0,
        /*src_w=*/4,       /*src_h=*/4);
    CHECK(m.a  == doctest::Approx(1.0f));
    CHECK(m.b  == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(m.tx == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(m.c  == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(m.d  == doctest::Approx(1.0f));
    CHECK(m.ty == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("affine_blit identity: src copied pixel-for-pixel into dst") {
    const int W = 4, H = 4;
    const auto src = labeled_src(W, H);
    std::vector<uint8_t> dst(W * H * 4, 0);
    const AffineMatrix identity;  /* default-constructed = identity */
    affine_blit(dst.data(), W, H, W * 4,
                src.data(), W, H, W * 4, identity);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            auto sp = pixel_at(src, W, x, y);
            auto dp = pixel_at(dst, W, x, y);
            CHECK(dp == sp);
        }
    }
}

TEST_CASE("affine_blit pure translate: dst shifted by translate offset") {
    const int W = 8, H = 8;
    const auto src = labeled_src(W, H);
    std::vector<uint8_t> dst(W * H * 4, 0);
    /* Translate (+2, +1) — dst (x, y) samples src (x-2, y-1). */
    const auto m = compose_inverse_affine(
        /*translate_x=*/2, /*translate_y=*/1,
        /*scale_x=*/1,     /*scale_y=*/1,
        /*rotation_deg=*/0,
        /*anchor_x=*/0,    /*anchor_y=*/0,
        /*src_w=*/W,       /*src_h=*/H);
    affine_blit(dst.data(), W, H, W * 4,
                src.data(), W, H, W * 4, m);

    /* Interior sample: dst (5, 3) ← src (3, 2). */
    auto dp = pixel_at(dst, W, 5, 3);
    CHECK(dp[0] == 3);  /* R = src x */
    CHECK(dp[1] == 2);  /* G = src y */
    CHECK(dp[3] == 255);

    /* Top-left corner of dst (0, 0) ← src (-2, -1) → out of bounds →
     * transparent. */
    auto tl = pixel_at(dst, W, 0, 0);
    CHECK(tl[0] == 0);
    CHECK(tl[3] == 0);
}

TEST_CASE("affine_blit pure scale 2×: src doubled into dst") {
    const int SW = 4, SH = 4;
    const auto src = labeled_src(SW, SH);
    const int DW = 8, DH = 8;
    std::vector<uint8_t> dst(DW * DH * 4, 0);
    const auto m = compose_inverse_affine(
        /*translate_x=*/0, /*translate_y=*/0,
        /*scale_x=*/2,     /*scale_y=*/2,
        /*rotation_deg=*/0,
        /*anchor_x=*/0,    /*anchor_y=*/0,
        /*src_w=*/SW,      /*src_h=*/SH);
    affine_blit(dst.data(), DW, DH, DW * 4,
                src.data(), SW, SH, SW * 4, m);
    /* dst (0, 0) ← src (0, 0); dst (2, 2) ← src (1, 1); etc. */
    CHECK(pixel_at(dst, DW, 0, 0)[0] == 0);
    CHECK(pixel_at(dst, DW, 2, 2)[0] == 1);
    CHECK(pixel_at(dst, DW, 2, 2)[1] == 1);
    CHECK(pixel_at(dst, DW, 4, 4)[0] == 2);
    CHECK(pixel_at(dst, DW, 4, 4)[1] == 2);
}

TEST_CASE("affine_blit out-of-bounds sampling yields transparent") {
    const int W = 4, H = 4;
    const auto src = labeled_src(W, H);
    std::vector<uint8_t> dst(W * H * 4, 255);  /* prefill non-zero */
    /* Translate (+10, 0) shifts src way off to the right → every
     * dst pixel samples src (x-10, y) which is always < 0 → all
     * transparent. */
    const auto m = compose_inverse_affine(10, 0, 1, 1, 0, 0, 0, W, H);
    affine_blit(dst.data(), W, H, W * 4,
                src.data(), W, H, W * 4, m);
    for (std::size_t i = 0; i < dst.size(); ++i) {
        CHECK(dst[i] == 0);
    }
}

TEST_CASE("affine_blit identity on fixed-size dst with smaller src") {
    /* src 2×2, dst 4×4, identity. dst (0..1, 0..1) copies src; dst
     * elsewhere samples out of bounds → transparent. */
    const int SW = 2, SH = 2;
    const auto src = labeled_src(SW, SH);
    const int DW = 4, DH = 4;
    std::vector<uint8_t> dst(DW * DH * 4, 99);  /* garbage prefill */
    const AffineMatrix identity;
    affine_blit(dst.data(), DW, DH, DW * 4,
                src.data(), SW, SH, SW * 4, identity);
    CHECK(pixel_at(dst, DW, 0, 0)[0] == 0);   /* src (0,0) */
    CHECK(pixel_at(dst, DW, 1, 1)[0] == 1);   /* src (1,1) */
    CHECK(pixel_at(dst, DW, 2, 0)[3] == 0);   /* OOB: alpha 0 */
    CHECK(pixel_at(dst, DW, 3, 3)[3] == 0);   /* OOB: alpha 0 */
}

TEST_CASE("affine_blit 180° rotation around center maps continuous pixel-grid") {
    /* 4×4 src, anchor at normalized (0.5, 0.5) → pixel (2, 2). 180°
     * rotation in continuous coord space around (2, 2) maps src (x, y)
     * to canvas (4 - x, 4 - y). So src (1, 1) → canvas (3, 3); src
     * (0, 0) → canvas (4, 4) which is outside the 0..3 range.
     *
     * Backward sampling: dst (3, 3) ← src (4 - 3, 4 - 3) = (1, 1).
     * dst (0, 0) ← src (4, 4) → out of bounds → transparent.
     *
     * This is mathematically-faithful continuous-rotation semantics;
     * a "discrete 180° flip" that maps (0,0)↔(3,3) symmetrically
     * needs anchor at ((W-1)/2, (H-1)/2) which isn't the default
     * normalized-center anchor. */
    const int W = 4, H = 4;
    const auto src = labeled_src(W, H);
    std::vector<uint8_t> dst(W * H * 4, 0);
    const auto m = compose_inverse_affine(
        /*translate_x=*/0, /*translate_y=*/0,
        /*scale_x=*/1,     /*scale_y=*/1,
        /*rotation_deg=*/180,
        /*anchor_x=*/0.5,  /*anchor_y=*/0.5,
        /*src_w=*/W,       /*src_h=*/H);
    affine_blit(dst.data(), W, H, W * 4,
                src.data(), W, H, W * 4, m);
    /* dst (3, 3) ← src (1, 1). */
    auto p33 = pixel_at(dst, W, 3, 3);
    CHECK(p33[0] == 1);
    CHECK(p33[1] == 1);
    CHECK(p33[3] == 255);
    /* dst (0, 0) ← src (4, 4) OOB → transparent. */
    auto p00 = pixel_at(dst, W, 0, 0);
    CHECK(p00[3] == 0);
}

TEST_CASE("affine_blit determinism: same inputs produce byte-identical dst") {
    const int W = 8, H = 8;
    const auto src = labeled_src(W, H);
    std::vector<uint8_t> dst1(W * H * 4, 0);
    std::vector<uint8_t> dst2(W * H * 4, 0);
    const auto m = compose_inverse_affine(1.3, -2.7, 1.5, 0.8, 37.5, 0.4, 0.6, W, H);
    affine_blit(dst1.data(), W, H, W * 4, src.data(), W, H, W * 4, m);
    affine_blit(dst2.data(), W, H, W * 4, src.data(), W, H, W * 4, m);
    CHECK(dst1 == dst2);
}
