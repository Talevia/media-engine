/*
 * test_inference_multiarray_layout — unit coverage for the layout
 * helpers used by `coreml_runtime.mm` to translate strided
 * MLMultiArray outputs into the engine's flat row-major Tensor
 * layout. Pure C++; no Apple frameworks needed (CoreML.framework
 * is what populates a real MLMultiArray, but the strided-copy
 * walk operates on plain byte buffers so it can be tested
 * independently — this is the whole reason the helpers were
 * factored out of the .mm).
 *
 * Gated on `ME_HAS_INFERENCE` (the helpers don't compile without
 * the engine's inference TUs in the link graph). Apple-specific
 * symbols don't appear here so the suite runs on any platform
 * that builds the engine with `ME_WITH_INFERENCE=ON`.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE
#include "inference/multiarray_layout.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

TEST_CASE("is_row_major_contiguous: scalar (empty shape) is contiguous") {
    std::array<std::int64_t, 0> shape{};
    std::array<std::int64_t, 0> strides{};
    CHECK(me::inference::is_row_major_contiguous(
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides)));
}

TEST_CASE("is_row_major_contiguous: 2D row-major matches") {
    /* shape=[2,3] row-major strides=[3,1]. */
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 2> strides{3, 1};
    CHECK(me::inference::is_row_major_contiguous(
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides)));
}

TEST_CASE("is_row_major_contiguous: 2D column-major-like rejected") {
    /* shape=[2,3] column-major strides=[1,2]. */
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 2> strides{1, 2};
    CHECK_FALSE(me::inference::is_row_major_contiguous(
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides)));
}

TEST_CASE("is_row_major_contiguous: length mismatch rejected") {
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 1> strides{3};
    CHECK_FALSE(me::inference::is_row_major_contiguous(
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides)));
}

TEST_CASE("is_row_major_contiguous: 4D NCHW row-major") {
    /* shape=[1,3,4,5] row-major strides=[60,20,5,1]. */
    const std::array<std::int64_t, 4> shape{1, 3, 4, 5};
    const std::array<std::int64_t, 4> strides{60, 20, 5, 1};
    CHECK(me::inference::is_row_major_contiguous(
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides)));
}

TEST_CASE("strided_copy_to_contiguous: 2D row-major identity round-trip") {
    /* When the source layout already matches contiguous row-major,
     * the strided walk produces identical bytes to a flat memcpy. */
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 2> strides{3, 1};
    const std::array<float, 6> src{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    std::array<float, 6> dst{};

    REQUIRE(me::inference::strided_copy_to_contiguous(
        reinterpret_cast<const std::uint8_t*>(src.data()),
        sizeof(src),
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides),
        sizeof(float),
        reinterpret_cast<std::uint8_t*>(dst.data())));

    for (std::size_t i = 0; i < src.size(); ++i) {
        CHECK(dst[i] == doctest::Approx(src[i]));
    }
}

TEST_CASE("strided_copy_to_contiguous: 2D column-major reorders to row-major") {
    /* Source is shape=[2,3] with column-major strides {1,2}: source
     * layout in memory is [r0c0, r1c0, r0c1, r1c1, r0c2, r1c2]. The
     * walk reorders to row-major dst: [r0c0, r0c1, r0c2, r1c0,
     * r1c1, r1c2]. */
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 2> strides{1, 2};  /* column-major */
    const std::array<float, 6> src{
        /* r0c0 */ 10.0f,
        /* r1c0 */ 40.0f,
        /* r0c1 */ 20.0f,
        /* r1c1 */ 50.0f,
        /* r0c2 */ 30.0f,
        /* r1c2 */ 60.0f,
    };
    std::array<float, 6> dst{};

    REQUIRE(me::inference::strided_copy_to_contiguous(
        reinterpret_cast<const std::uint8_t*>(src.data()),
        sizeof(src),
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides),
        sizeof(float),
        reinterpret_cast<std::uint8_t*>(dst.data())));

    /* Expected row-major output. */
    const std::array<float, 6> expected{
        10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f,
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(dst[i] == doctest::Approx(expected[i]));
    }
}

TEST_CASE("strided_copy_to_contiguous: padded row stride") {
    /* shape=[2,3], element-stride=[4,1] — i.e., each row has 1 extra
     * element of padding before the next row begins. Source needs
     * 7 elements: 3 row-0, 1 padding, 3 row-1. */
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 2> strides{4, 1};
    const std::array<std::int32_t, 7> src{
        /* row 0 */ 11, 22, 33,
        /* pad   */ 99,
        /* row 1 */ 44, 55, 66,
    };
    std::array<std::int32_t, 6> dst{};

    REQUIRE(me::inference::strided_copy_to_contiguous(
        reinterpret_cast<const std::uint8_t*>(src.data()),
        sizeof(src),
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides),
        sizeof(std::int32_t),
        reinterpret_cast<std::uint8_t*>(dst.data())));

    const std::array<std::int32_t, 6> expected{11, 22, 33, 44, 55, 66};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(dst[i] == expected[i]);
    }
}

TEST_CASE("strided_copy_to_contiguous: rejects insufficient src_byte_size") {
    /* If the caller hands in a src buffer too small for the implied
     * maximum offset, return false rather than reading out of bounds. */
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 2> strides{3, 1};
    const std::array<float, 6> src{};
    std::array<float, 6> dst{};

    /* 5 floats < 6 → must reject. */
    CHECK_FALSE(me::inference::strided_copy_to_contiguous(
        reinterpret_cast<const std::uint8_t*>(src.data()),
        sizeof(float) * 5,
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides),
        sizeof(float),
        reinterpret_cast<std::uint8_t*>(dst.data())));
}

TEST_CASE("strided_copy_to_contiguous: rejects shape/strides length mismatch") {
    const std::array<std::int64_t, 2> shape{2, 3};
    const std::array<std::int64_t, 1> strides{3};
    const std::array<float, 6> src{};
    std::array<float, 6> dst{};

    CHECK_FALSE(me::inference::strided_copy_to_contiguous(
        reinterpret_cast<const std::uint8_t*>(src.data()),
        sizeof(src),
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides),
        sizeof(float),
        reinterpret_cast<std::uint8_t*>(dst.data())));
}

TEST_CASE("strided_copy_to_contiguous: 3D permuted axes") {
    /* shape=[2,2,2], strides=[2,4,1] — axis-1 outer, axis-0 inner.
     * That's a permutation of row-major (which would be {4,2,1}).
     *
     * Source memory layout (8 elements, 0..7):
     *   addr 0 = (i0=0, i1=0, i2=0)  src offset = 0*2 + 0*4 + 0 = 0
     *   addr 1 = (i0=0, i1=0, i2=1)  src offset = 0 + 0 + 1     = 1
     *   addr 2 = (i0=1, i1=0, i2=0)  src offset = 2 + 0 + 0     = 2
     *   addr 3 = (i0=1, i1=0, i2=1)  src offset = 2 + 0 + 1     = 3
     *   addr 4 = (i0=0, i1=1, i2=0)  src offset = 0 + 4 + 0     = 4
     *   addr 5 = (i0=0, i1=1, i2=1)  src offset = 0 + 4 + 1     = 5
     *   addr 6 = (i0=1, i1=1, i2=0)  src offset = 2 + 4 + 0     = 6
     *   addr 7 = (i0=1, i1=1, i2=1)  src offset = 2 + 4 + 1     = 7
     *
     * Pack source so addr matches value × 10. Walk row-major dst —
     * lin order (i0, i1, i2): [(0,0,0),(0,0,1),(0,1,0),(0,1,1),
     *                            (1,0,0),(1,0,1),(1,1,0),(1,1,1)]
     * which maps to source addresses: 0, 1, 4, 5, 2, 3, 6, 7 →
     * values 0, 10, 40, 50, 20, 30, 60, 70. */
    const std::array<std::int64_t, 3> shape{2, 2, 2};
    const std::array<std::int64_t, 3> strides{2, 4, 1};
    const std::array<std::int32_t, 8> src{0, 10, 20, 30, 40, 50, 60, 70};
    std::array<std::int32_t, 8> dst{};

    REQUIRE(me::inference::strided_copy_to_contiguous(
        reinterpret_cast<const std::uint8_t*>(src.data()),
        sizeof(src),
        std::span<const std::int64_t>(shape),
        std::span<const std::int64_t>(strides),
        sizeof(std::int32_t),
        reinterpret_cast<std::uint8_t*>(dst.data())));

    const std::array<std::int32_t, 8> expected{0, 10, 40, 50, 20, 30, 60, 70};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(dst[i] == expected[i]);
    }
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("multiarray_layout: skipped (ME_WITH_INFERENCE=OFF)") {
    /* Build-flag-gated stub. */
}

#endif
