/* decode_selfie_segmentation_mask — turn the SelfieSegmentation
 * model's float-logit output into an RGBA-compatible uint8
 * alpha plane sized to the caller's target dimensions.
 *
 * MediaPipe SelfieSegmentation produces a per-pixel logit
 * (typically shape `{1, 1, H, W}` NCHW or `{1, H, W, 1}` NHWC,
 * `H` / `W` matching the model's input — 256 for the canonical
 * "general" variant). The decoder applies:
 *
 *   1. sigmoid(logit) → probability ∈ [0, 1]
 *   2. quantize to uint8: `round_half_up(prob * 255)`
 *   3. bilinear upscale from (H, W) to (target_width, target_height)
 *
 * The output alpha plane is a tightly-packed `target_w * target_h`
 * uint8 buffer suitable for a body-alpha-key composition stage.
 *
 * Determinism caveat (matches existing project precedent for
 * float-using effect kernels): the float math is IEEE-754
 * single-precision; same compiler + arch combination yields
 * the same bytes. Cross-compiler bit-equality is not guaranteed.
 *
 * Argument-shape rejects:
 *   - logits.dtype != Float32                        → ME_E_INVALID_ARG
 *   - logits.shape outside {1, 1, H, W} or
 *     {1, H, W, 1} with H, W > 0                     → ME_E_INVALID_ARG
 *   - logits.bytes.size() != H * W * 4               → ME_E_INVALID_ARG
 *   - target_width <= 0 / target_height <= 0         → ME_E_INVALID_ARG
 *   - out_mask_width == nullptr / out_mask_height
 *     == nullptr / out_alpha == nullptr              → ME_E_INVALID_ARG
 */
#pragma once

#include "inference/runtime.hpp"
#include "media_engine/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace me::compose {

me_status_t decode_selfie_segmentation_mask(
    const me::inference::Tensor& logits,
    int                          target_width,
    int                          target_height,
    int*                         out_mask_width,
    int*                         out_mask_height,
    std::vector<std::uint8_t>*   out_alpha,
    std::string*                 err);

}  // namespace me::compose
