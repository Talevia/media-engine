/* Inference input preprocessor — RGBA frame → planar CHW
 * float32 tensor sized to a model's input shape.
 *
 * Two M11 ML pipelines (BlazeFace face detection, MediaPipe
 * SelfieSegmentation) share the same preprocessing structure:
 *
 *   1. libswscale resize from caller (width, height) RGBA →
 *      (target_w, target_h) RGBA. AREA filter (well-defined
 *      deterministic output across libswscale versions for the
 *      dimensions we target).
 *   2. RGBA → planar CHW float32 (R-plane, G-plane, B-plane).
 *      Alpha discarded.
 *   3. Per-pixel float math: `f = byte * scale + bias`.
 *
 * Models differ on (target_w, target_h, scale, bias) — for
 * BlazeFace it's (128, 128, 1/127.5, -1.0) producing [-1, 1];
 * for SelfieSegmentation it's (256, 256, 1/255, 0) producing
 * [0, 1]. The generic helper `prepare_inference_input` takes
 * those four values directly; `prepare_blazeface_input` and
 * `prepare_selfie_segmentation_input` are thin wrappers that
 * fix the four values per model.
 *
 * Determinism caveat (matches existing project precedent for
 * float-using effect kernels): libswscale's resize math and
 * the float multiply/add are IEEE-754 single-precision; same
 * compiler + arch combination yields the same bytes. Cross-
 * compiler bit-equality is not guaranteed.
 *
 * Argument-shape rejects (apply to all three entry points):
 *   - rgba == nullptr / width <= 0 / height <= 0  → ME_E_INVALID_ARG
 *   - stride_bytes < width * 4                     → ME_E_INVALID_ARG
 *   - target_w <= 0 / target_h <= 0                → ME_E_INVALID_ARG
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

/* Documented model input shapes (M11). */
constexpr int kBlazefaceInputDim          = 128;
constexpr int kSelfieSegmentationInputDim = 256;

/* Generic helper: target dims + (scale, bias) baked at the
 * call site. Output tensor has shape {1, 3, target_h, target_w}
 * and dtype Float32. Pixel layout is NCHW: R-plane first, then
 * G-plane, then B-plane. */
me_status_t prepare_inference_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    int                    target_w,
    int                    target_h,
    float                  scale,
    float                  bias,
    me::inference::Tensor* out,
    std::string*           err);

/* BlazeFace face-detection preprocessor: 128×128, [-1, 1]
 * normalization (`byte / 127.5 - 1.0`). */
me_status_t prepare_blazeface_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    me::inference::Tensor* out,
    std::string*           err);

/* MediaPipe SelfieSegmentation preprocessor: 256×256, [0, 1]
 * normalization (`byte / 255.0`). */
me_status_t prepare_selfie_segmentation_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    me::inference::Tensor* out,
    std::string*           err);

}  // namespace me::compose
