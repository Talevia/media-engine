#include "inference/multiarray_layout.hpp"

#include <cstring>
#include <vector>

namespace me::inference {

bool is_row_major_contiguous(
    std::span<const std::int64_t> shape,
    std::span<const std::int64_t> element_strides) {
    if (shape.size() != element_strides.size()) return false;
    if (shape.empty()) return true;
    std::int64_t expected = 1;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(shape.size()) - 1;
         i >= 0; --i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        if (element_strides[idx] != expected) return false;
        expected *= shape[idx];
    }
    return true;
}

bool strided_copy_to_contiguous(
    const std::uint8_t*           src,
    std::size_t                   src_byte_size,
    std::span<const std::int64_t> shape,
    std::span<const std::int64_t> element_strides,
    std::size_t                   elem_bytes,
    std::uint8_t*                 dst) {

    if (shape.size() != element_strides.size()) return false;
    if (!src || !dst || elem_bytes == 0) return false;
    for (std::int64_t d : shape) {
        if (d < 0) return false;
    }
    /* Element count + per-axis row-major denominators in one
     * pass: denom[i] = product(shape[i+1..]). Empty shape ⇒
     * single scalar element, copy elem_bytes directly. */
    const std::size_t ndims = shape.size();
    std::size_t       elem_count = 1;
    for (std::int64_t d : shape) elem_count *= static_cast<std::size_t>(d);

    if (elem_count == 0) return true;  /* zero-size tensor — nothing to copy */

    std::vector<std::int64_t> denom(ndims, 1);
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(ndims) - 2;
         i >= 0; --i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        denom[idx] = denom[idx + 1] * shape[idx + 1];
    }

    /* Compute the maximum source byte offset accessed so we can
     * bounds-check `src_byte_size` upfront. The maximum offset
     * for a strided layout is sum_i (shape[i]-1) * element_strides[i]
     * elements from src — for any non-degenerate axis. */
    std::int64_t max_src_off_elements = 0;
    for (std::size_t i = 0; i < ndims; ++i) {
        if (shape[i] > 0) {
            max_src_off_elements += (shape[i] - 1) * element_strides[i];
        }
    }
    const std::size_t max_src_byte_off =
        static_cast<std::size_t>(max_src_off_elements) * elem_bytes;
    if (max_src_byte_off + elem_bytes > src_byte_size) return false;

    for (std::size_t lin = 0; lin < elem_count; ++lin) {
        std::size_t  remaining   = lin;
        std::int64_t src_off_el  = 0;
        for (std::size_t d = 0; d < ndims; ++d) {
            const std::size_t denom_d  = static_cast<std::size_t>(denom[d]);
            const std::size_t coord_d  = remaining / denom_d;
            remaining                  -= coord_d * denom_d;
            src_off_el +=
                static_cast<std::int64_t>(coord_d) * element_strides[d];
        }
        std::memcpy(
            dst + lin * elem_bytes,
            src + static_cast<std::size_t>(src_off_el) * elem_bytes,
            elem_bytes);
    }
    return true;
}

}  // namespace me::inference
