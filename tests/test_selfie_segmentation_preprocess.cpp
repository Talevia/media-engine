/* test_selfie_segmentation_preprocess — covers
 * `prepare_selfie_segmentation_input`, the libswscale-based
 * RGBA → 256×256×3 NCHW float32 [0, 1] preprocessor wired
 * from the SelfieSegmentation runtime mask resolver.
 */
#include <doctest/doctest.h>

#include "compose/inference_input.hpp"
#include "inference/runtime.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr int N = 256;

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

TEST_CASE("prepare_selfie_segmentation_input: null buffer rejected") {
    me::inference::Tensor t;
    std::string err;
    CHECK(me::compose::prepare_selfie_segmentation_input(
              nullptr, 256, 256, 256 * 4, &t, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("prepare_selfie_segmentation_input: shape + dtype documented") {
    auto buf = make_solid(512, 512, 512 * 4, 100, 100, 100, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_selfie_segmentation_input(
                buf.data(), 512, 512, 512 * 4, &t, nullptr) == ME_OK);
    REQUIRE(t.shape.size() == 4);
    CHECK(t.shape[0] == 1);
    CHECK(t.shape[1] == 3);
    CHECK(t.shape[2] == N);
    CHECK(t.shape[3] == N);
    CHECK(t.dtype == me::inference::Dtype::Float32);
    CHECK(t.bytes.size() == static_cast<std::size_t>(1) * 3 * N * N * 4);
}

TEST_CASE("prepare_selfie_segmentation_input: solid 0 → all values = 0.0") {
    auto buf = make_solid(64, 64, 64 * 4, 0, 0, 0, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_selfie_segmentation_input(
                buf.data(), 64, 64, 64 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    for (std::size_t i = 0; i < static_cast<std::size_t>(3) * N * N; ++i) {
        CHECK(fp[i] == doctest::Approx(0.0f).epsilon(1e-5f));
    }
}

TEST_CASE("prepare_selfie_segmentation_input: solid 255 → all values = 1.0") {
    auto buf = make_solid(64, 64, 64 * 4, 255, 255, 255, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_selfie_segmentation_input(
                buf.data(), 64, 64, 64 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    for (std::size_t i = 0; i < static_cast<std::size_t>(3) * N * N; ++i) {
        CHECK(fp[i] == doctest::Approx(1.0f).epsilon(1e-5f));
    }
}

TEST_CASE("prepare_selfie_segmentation_input: solid mid-gray (128) → ≈ 0.502") {
    /* 128 / 255 = 0.5019607... per channel, every pixel. */
    auto buf = make_solid(256, 256, 256 * 4, 128, 128, 128, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_selfie_segmentation_input(
                buf.data(), 256, 256, 256 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    const float expected = 128.0f / 255.0f;
    for (std::size_t i = 0; i < static_cast<std::size_t>(3) * N * N; ++i) {
        CHECK(std::fabs(fp[i] - expected) < 1e-4f);
    }
}

TEST_CASE("prepare_selfie_segmentation_input: planar layout is RGB (alpha discarded)") {
    auto buf = make_solid(64, 64, 64 * 4, 200, 100, 50, 99);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_selfie_segmentation_input(
                buf.data(), 64, 64, 64 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    const std::size_t plane = static_cast<std::size_t>(N) * N;
    const std::size_t mid = (N / 2) * N + N / 2;
    CHECK(fp[0 * plane + mid] == doctest::Approx(200.0f / 255.0f).epsilon(1e-3f));
    CHECK(fp[1 * plane + mid] == doctest::Approx(100.0f / 255.0f).epsilon(1e-3f));
    CHECK(fp[2 * plane + mid] == doctest::Approx(50.0f  / 255.0f).epsilon(1e-3f));
}

TEST_CASE("prepare_inference_input: generic helper rejects bad target dims") {
    auto buf = make_solid(64, 64, 64 * 4, 100, 100, 100, 255);
    me::inference::Tensor t;
    CHECK(me::compose::prepare_inference_input(
              buf.data(), 64, 64, 64 * 4,
              0, 64, 1.0f, 0.0f, &t, nullptr) == ME_E_INVALID_ARG);
    CHECK(me::compose::prepare_inference_input(
              buf.data(), 64, 64, 64 * 4,
              64, -1, 1.0f, 0.0f, &t, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("prepare_inference_input: generic helper accepts arbitrary target dims") {
    auto buf = make_solid(96, 96, 96 * 4, 200, 100, 50, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_inference_input(
                buf.data(), 96, 96, 96 * 4,
                64, 32, 1.0f / 255.0f, 0.0f, &t, nullptr) == ME_OK);
    REQUIRE(t.shape.size() == 4);
    CHECK(t.shape[0] == 1);
    CHECK(t.shape[1] == 3);
    CHECK(t.shape[2] == 32);
    CHECK(t.shape[3] == 64);
    CHECK(t.bytes.size() == static_cast<std::size_t>(1) * 3 * 32 * 64 * 4);
}
