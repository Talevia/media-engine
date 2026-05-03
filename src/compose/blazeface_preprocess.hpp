/* prepare_blazeface_input — frame preprocessing for the
 * BlazeFace runtime input tensor.
 *
 * BlazeFace's documented input shape is `1 × 3 × 128 × 128`
 * float32 (NCHW), normalized to [-1, 1]. This helper takes a
 * caller-owned RGBA8 frame (any dimensions, may be padded) and
 * produces a `me::inference::Tensor` matching that shape:
 *
 *   1. libswscale resize from (width, height) RGBA →
 *      (128, 128) RGBA. AREA filter (well-defined deterministic
 *      output across libswscale versions for the dimensions we
 *      target).
 *   2. RGBA → planar CHW float32 with `value / 127.5 - 1.0`
 *      per channel (R, G, B; alpha discarded).
 *
 * Determinism caveat (matches existing project precedent for
 * float-using effect kernels): libswscale's resize math and the
 * float division are IEEE-754 single-precision; same compiler
 * + arch combination yields the same bytes. Cross-compiler
 * bit-equality is not guaranteed.
 *
 * Argument-shape rejects:
 *   - rgba == nullptr / width <= 0 / height <= 0  → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4                     → ME_E_INVALID_ARG
 *   - out == nullptr                               → ME_E_INVALID_ARG
 *
 * On non-OK return `*out` is left in an unspecified state; the
 * caller should not consume it.
 */
#pragma once

#include "inference/runtime.hpp"
#include "media_engine/types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace me::compose {

constexpr int kBlazefaceInputDim = 128;

me_status_t prepare_blazeface_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    me::inference::Tensor* out,
    std::string*           err);

}  // namespace me::compose
