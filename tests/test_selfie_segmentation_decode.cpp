/* test_selfie_segmentation_decode — covers the
 * `decode_selfie_segmentation_mask` SelfieSegmentation logit →
 * uint8 alpha decoder. */
#include <doctest/doctest.h>

#include "compose/selfie_segmentation_decode.hpp"
#include "inference/runtime.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

me::inference::Tensor make_logits_nchw(int H, int W,
                                          const std::vector<float>& vals) {
    REQUIRE(static_cast<int>(vals.size()) == H * W);
    me::inference::Tensor t;
    t.shape = { 1, 1, H, W };
    t.dtype = me::inference::Dtype::Float32;
    t.bytes.assign(static_cast<std::size_t>(H) * W * 4, 0);
    std::memcpy(t.bytes.data(), vals.data(), vals.size() * 4);
    return t;
}

me::inference::Tensor make_logits_nhwc(int H, int W,
                                          const std::vector<float>& vals) {
    REQUIRE(static_cast<int>(vals.size()) == H * W);
    me::inference::Tensor t;
    t.shape = { 1, H, W, 1 };
    t.dtype = me::inference::Dtype::Float32;
    t.bytes.assign(static_cast<std::size_t>(H) * W * 4, 0);
    std::memcpy(t.bytes.data(), vals.data(), vals.size() * 4);
    return t;
}

}  // namespace

TEST_CASE("decode_selfie_segmentation_mask: null out args rejected") {
    auto t = make_logits_nchw(2, 2, {0.f, 0.f, 0.f, 0.f});
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    std::string err;
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 4, 4, nullptr, &mh, &alpha, &err) == ME_E_INVALID_ARG);
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 4, 4, &mw, nullptr, &alpha, &err) == ME_E_INVALID_ARG);
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 4, 4, &mw, &mh, nullptr, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_selfie_segmentation_mask: bad target dims rejected") {
    auto t = make_logits_nchw(2, 2, {0.f, 0.f, 0.f, 0.f});
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 0, 4, &mw, &mh, &alpha, nullptr) == ME_E_INVALID_ARG);
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 4, -1, &mw, &mh, &alpha, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_selfie_segmentation_mask: non-Float32 dtype rejected") {
    me::inference::Tensor t;
    t.shape = { 1, 1, 2, 2 };
    t.dtype = me::inference::Dtype::Uint8;
    t.bytes.assign(4, 0);
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    std::string err;
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 4, 4, &mw, &mh, &alpha, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_selfie_segmentation_mask: unsupported shape rejected") {
    me::inference::Tensor t;
    t.shape = { 1, 3, 2, 2 };  /* multi-channel, NCHW with C != 1 */
    t.dtype = me::inference::Dtype::Float32;
    t.bytes.assign(static_cast<std::size_t>(1) * 3 * 2 * 2 * 4, 0);
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 4, 4, &mw, &mh, &alpha, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_selfie_segmentation_mask: byte-size mismatch rejected") {
    me::inference::Tensor t;
    t.shape = { 1, 1, 4, 4 };
    t.dtype = me::inference::Dtype::Float32;
    t.bytes.assign(8, 0);  /* expected 4*4*4 = 64; supplied 8 */
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    CHECK(me::compose::decode_selfie_segmentation_mask(
              t, 8, 8, &mw, &mh, &alpha, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_selfie_segmentation_mask: logit=0 → alpha=128 (sigmoid(0)=0.5)") {
    auto t = make_logits_nchw(4, 4, std::vector<float>(16, 0.f));
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 4, 4, &mw, &mh, &alpha, nullptr) == ME_OK);
    CHECK(mw == 4);
    CHECK(mh == 4);
    REQUIRE(alpha.size() == 16);
    /* round_half_up(0.5 * 255) = 128. */
    for (auto v : alpha) CHECK(v == 128);
}

TEST_CASE("decode_selfie_segmentation_mask: large +logit → alpha=255") {
    auto t = make_logits_nchw(4, 4, std::vector<float>(16, 20.f));
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 4, 4, &mw, &mh, &alpha, nullptr) == ME_OK);
    /* sigmoid(20) ≈ 1.0 → alpha = 255. */
    for (auto v : alpha) CHECK(v == 255);
}

TEST_CASE("decode_selfie_segmentation_mask: large -logit → alpha=0") {
    auto t = make_logits_nchw(4, 4, std::vector<float>(16, -20.f));
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 4, 4, &mw, &mh, &alpha, nullptr) == ME_OK);
    /* sigmoid(-20) ≈ 0.0 → alpha = 0. */
    for (auto v : alpha) CHECK(v == 0);
}

TEST_CASE("decode_selfie_segmentation_mask: NHWC layout accepted") {
    /* Same data as NCHW logit=0 case; layout {1, H, W, 1}. */
    auto t = make_logits_nhwc(4, 4, std::vector<float>(16, 0.f));
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 4, 4, &mw, &mh, &alpha, nullptr) == ME_OK);
    for (auto v : alpha) CHECK(v == 128);
}

TEST_CASE("decode_selfie_segmentation_mask: bilinear upscale produces full target") {
    /* 2×2 logit → 8×8 target. Center logit values create a
     * gradient when upscaled. */
    std::vector<float> vals = {
        -10.f, -10.f,
        +10.f, +10.f,
    };
    auto t = make_logits_nchw(2, 2, vals);
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 8, 8, &mw, &mh, &alpha, nullptr) == ME_OK);
    CHECK(mw == 8);
    CHECK(mh == 8);
    REQUIRE(alpha.size() == 64);
    /* Top row (corresponds to source y=0): all values near 0
     * (sigmoid(-10) ≈ 0). Bottom row: all near 255. */
    for (int x = 0; x < 8; ++x) {
        CHECK(alpha[0 * 8 + x] <= 5);
        CHECK(alpha[7 * 8 + x] >= 250);
    }
}

TEST_CASE("decode_selfie_segmentation_mask: 1:1 source = target → no resample") {
    /* Source and target dims match → output should equal the
     * sigmoid + quantize step bit-for-bit (no bilinear). */
    std::vector<float> vals = {
        -2.f,  0.f,  2.f,  0.f,
         0.f,  2.f, -2.f,  0.f,
         0.f,  0.f,  0.f,  0.f,
         2.f, -2.f,  0.f,  2.f,
    };
    auto t = make_logits_nchw(4, 4, vals);
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 4, 4, &mw, &mh, &alpha, nullptr) == ME_OK);
    REQUIRE(alpha.size() == 16);
    /* sigmoid(2) ≈ 0.881; round to 225. sigmoid(-2) ≈ 0.119;
     * round to 30. sigmoid(0) = 0.5 → 128. Just verify the
     * relative order. */
    CHECK(alpha[0]  == 30);   /* logit=-2 */
    CHECK(alpha[2]  == 225);  /* logit=+2 */
    CHECK(alpha[1]  == 128);  /* logit=0  */
}

TEST_CASE("decode_selfie_segmentation_mask: determinism") {
    std::vector<float> vals(64);
    for (std::size_t i = 0; i < vals.size(); ++i) {
        vals[i] = static_cast<float>(i) * 0.1f - 3.0f;
    }
    auto t = make_logits_nchw(8, 8, vals);
    int mw_a = 0, mh_a = 0, mw_b = 0, mh_b = 0;
    std::vector<std::uint8_t> a, b;
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 16, 16, &mw_a, &mh_a, &a, nullptr) == ME_OK);
    REQUIRE(me::compose::decode_selfie_segmentation_mask(
                t, 16, 16, &mw_b, &mh_b, &b, nullptr) == ME_OK);
    CHECK(a == b);
    CHECK(mw_a == mw_b);
    CHECK(mh_a == mh_b);
}
