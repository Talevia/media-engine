/* MLMultiArray layout helpers — pure C++, no Apple-framework
 * dependency. Used by `coreml_runtime.mm` to translate a CoreML
 * MLMultiArray output (which may have non-contiguous element-
 * strides) into the engine's flat row-major Tensor::bytes layout.
 *
 * Why factored out: the `.mm` translation unit can't be linked
 * into a unit test without dragging Apple frameworks in. By
 * pulling the layout math into plain C++ here, tests can drive
 * `strided_copy_to_contiguous` with hand-crafted byte buffers +
 * stride arrays and verify the walk's correctness independently
 * of CoreML.framework.
 *
 * Element-stride convention. CoreML's `MLMultiArray.strides`
 * publishes strides in elements, not bytes (see Apple docs for
 * MLMultiArray). The functions here take element-strides
 * directly and multiply by `elem_bytes` internally so callers
 * pass the array as Apple hands it over.
 *
 * Domain. Strides may be any non-negative int64 (Apple supports
 * stride arrays where dimension-axis strides aren't sorted by
 * descending magnitude — the layout is fully strided not just
 * "permuted-contiguous"). All functions are deterministic; no
 * SIMD, no parallelism.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace me::inference {

/* Test whether (`shape`, `element_strides`) describes a row-major
 * contiguous layout, i.e. element-stride[i] == product(shape[i+1..]).
 * Returns false on mismatched lengths or any per-axis stride
 * mismatch. The fast-path memcpy is only legal when this returns
 * true. */
bool is_row_major_contiguous(
    std::span<const std::int64_t> shape,
    std::span<const std::int64_t> element_strides);

/* Walk a strided source layout into a contiguous row-major
 * destination. `dst` must have at least `product(shape) *
 * elem_bytes` bytes available. `src` must cover the maximum
 * source offset implied by (`shape`, `element_strides`,
 * `elem_bytes`); callers pass the tightest bound from Apple's
 * getBytesWithHandler block. Returns false if argument shapes /
 * strides don't agree (length mismatch, negative dim).
 *
 * Algorithmic. For each linear destination index L in [0,
 * product(shape)), reconstruct the multi-index (i0, .., in-1)
 * via row-major decomposition, compute source element offset
 * `sum(ij * stride_j)`, copy `elem_bytes` bytes from
 * `src + src_off * elem_bytes` to `dst + L * elem_bytes`. O(N)
 * memcpys; same byte count as a flat memcpy but with per-
 * element address arithmetic. */
bool strided_copy_to_contiguous(
    const std::uint8_t*           src,
    std::size_t                   src_byte_size,
    std::span<const std::int64_t> shape,
    std::span<const std::int64_t> element_strides,
    std::size_t                   elem_bytes,
    std::uint8_t*                 dst);

}  // namespace me::inference
