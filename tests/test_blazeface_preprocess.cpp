/* test_blazeface_preprocess — covers prepare_blazeface_input,
 * the libswscale-based RGBA → 128×128×3 NCHW float32 [-1, 1]
 * preprocessor wired from the BlazeFace runtime resolver.
 */
#include <doctest/doctest.h>

#include "compose/blazeface_preprocess.hpp"
#include "inference/runtime.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr int N = 128;

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

TEST_CASE("prepare_blazeface_input: null buffer rejected") {
    me::inference::Tensor t;
    std::string err;
    CHECK(me::compose::prepare_blazeface_input(
              nullptr, 256, 256, 256 * 4, &t, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("prepare_blazeface_input: bad dims rejected") {
    auto buf = make_solid(8, 8, 32, 0, 0, 0, 255);
    me::inference::Tensor t;
    CHECK(me::compose::prepare_blazeface_input(
              buf.data(), 0, 8, 32, &t, nullptr) == ME_E_INVALID_ARG);
    CHECK(me::compose::prepare_blazeface_input(
              buf.data(), 8, -1, 32, &t, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("prepare_blazeface_input: stride too small rejected") {
    auto buf = make_solid(16, 16, 64, 0, 0, 0, 255);
    me::inference::Tensor t;
    CHECK(me::compose::prepare_blazeface_input(
              buf.data(), 16, 16, 32, &t, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("prepare_blazeface_input: null out rejected") {
    auto buf = make_solid(16, 16, 64, 0, 0, 0, 255);
    CHECK(me::compose::prepare_blazeface_input(
              buf.data(), 16, 16, 64, nullptr, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("prepare_blazeface_input: shape + dtype + size are documented") {
    auto buf = make_solid(256, 256, 256 * 4, 100, 100, 100, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_blazeface_input(
                buf.data(), 256, 256, 256 * 4, &t, nullptr) == ME_OK);
    REQUIRE(t.shape.size() == 4);
    CHECK(t.shape[0] == 1);
    CHECK(t.shape[1] == 3);
    CHECK(t.shape[2] == N);
    CHECK(t.shape[3] == N);
    CHECK(t.dtype == me::inference::Dtype::Float32);
    CHECK(t.bytes.size() == static_cast<std::size_t>(1) * 3 * N * N * 4);
}

TEST_CASE("prepare_blazeface_input: solid mid-gray (128) → all values ≈ 0.004") {
    /* 128 / 127.5 - 1.0 = 0.00392... per channel, every pixel.
     * Verify the entire planar buffer is within ±1e-4 of that. */
    auto buf = make_solid(256, 256, 256 * 4, 128, 128, 128, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_blazeface_input(
                buf.data(), 256, 256, 256 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    const float expected = 128.0f / 127.5f - 1.0f;
    for (std::size_t i = 0; i < static_cast<std::size_t>(3) * N * N; ++i) {
        CHECK(std::fabs(fp[i] - expected) < 1e-4f);
    }
}

TEST_CASE("prepare_blazeface_input: solid 0 → all values = -1.0") {
    auto buf = make_solid(64, 64, 64 * 4, 0, 0, 0, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_blazeface_input(
                buf.data(), 64, 64, 64 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    for (std::size_t i = 0; i < static_cast<std::size_t>(3) * N * N; ++i) {
        CHECK(fp[i] == doctest::Approx(-1.0f).epsilon(1e-5f));
    }
}

TEST_CASE("prepare_blazeface_input: solid 255 → all values = +1.0") {
    auto buf = make_solid(64, 64, 64 * 4, 255, 255, 255, 255);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_blazeface_input(
                buf.data(), 64, 64, 64 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    /* 255/127.5 - 1.0 = 1.0. */
    for (std::size_t i = 0; i < static_cast<std::size_t>(3) * N * N; ++i) {
        CHECK(fp[i] == doctest::Approx(1.0f).epsilon(1e-5f));
    }
}

TEST_CASE("prepare_blazeface_input: planar layout is RGB (alpha discarded)") {
    /* Build an image with R=200, G=100, B=50, A=99. After
     * preprocessing, the R-plane should be ((200/127.5 - 1) =
     * 0.5686), G-plane ≈ -0.2156, B-plane ≈ -0.6078. The alpha
     * channel must NOT appear anywhere in the output. */
    auto buf = make_solid(64, 64, 64 * 4, 200, 100, 50, 99);
    me::inference::Tensor t;
    REQUIRE(me::compose::prepare_blazeface_input(
                buf.data(), 64, 64, 64 * 4, &t, nullptr) == ME_OK);
    const auto* fp = reinterpret_cast<const float*>(t.bytes.data());
    const std::size_t plane = static_cast<std::size_t>(N) * N;
    /* Sample one pixel from each plane (center). */
    const std::size_t mid = (N / 2) * N + N / 2;
    CHECK(fp[0 * plane + mid] == doctest::Approx(200.0f / 127.5f - 1.0f).epsilon(1e-3f));
    CHECK(fp[1 * plane + mid] == doctest::Approx(100.0f / 127.5f - 1.0f).epsilon(1e-3f));
    CHECK(fp[2 * plane + mid] == doctest::Approx(50.0f  / 127.5f - 1.0f).epsilon(1e-3f));
}

TEST_CASE("prepare_blazeface_input: determinism") {
    auto buf = make_solid(96, 96, 96 * 4, 80, 160, 240, 255);
    me::inference::Tensor a, b;
    REQUIRE(me::compose::prepare_blazeface_input(
                buf.data(), 96, 96, 96 * 4, &a, nullptr) == ME_OK);
    REQUIRE(me::compose::prepare_blazeface_input(
                buf.data(), 96, 96, 96 * 4, &b, nullptr) == ME_OK);
    CHECK(a.bytes == b.bytes);
}
