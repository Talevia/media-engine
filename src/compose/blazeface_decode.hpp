/* decode_blazeface_bboxes — turn BlazeFace's two output
 * tensors (per-anchor regression + per-anchor confidence
 * logit) into a list of frame-space bounding boxes.
 *
 * BlazeFace is the canonical short-range face detector
 * (MediaPipe `face_detection_short_range.tflite`, BlazeFace
 * paper). The 128×128 front-camera variant emits:
 *
 *   regressors:    {1, 896, 16}  per-anchor (dx, dy, dw, dh,
 *                                  + 6 keypoints * (kx, ky))
 *   classificators: {1, 896, 1}  per-anchor confidence logit
 *
 * 896 anchors come from two grids (16×16 with 2 anchors / cell
 * = 512, plus 8×8 with 6 anchors / cell = 384).
 *
 * Pipeline:
 *   1. Generate anchor centers + sizes for the documented
 *      MediaPipe FrontCamera config (hardcoded; see .cpp).
 *   2. For each anchor, sigmoid(confidence) ≥ confidence_threshold
 *      → decode the bbox: cx = anchor_cx + dx / input_w,
 *      cy = anchor_cy + dy / input_h, w = dw / input_w,
 *      h = dh / input_h.
 *   3. Convert from center-form to corner-form, clamp to [0, 1],
 *      then scale to (frame_width, frame_height) pixel coords.
 *   4. Greedy NMS by descending confidence with IoU threshold.
 *
 * Determinism caveat (matches existing project precedent for
 * float-using effect kernels): the float math is IEEE-754
 * single-precision; same compiler + arch combination yields
 * the same bytes. Cross-compiler bit-equality is not
 * guaranteed.
 *
 * Argument-shape rejects:
 *   - regressors / classificators dtype != Float32 → ME_E_INVALID_ARG
 *   - regressors.shape doesn't match {1, 896, 16}  → ME_E_INVALID_ARG
 *   - classificators.shape doesn't match {1, 896, 1} or
 *     {1, 896}                                       → ME_E_INVALID_ARG
 *   - tensor bytes don't match shape product * 4    → ME_E_INVALID_ARG
 *   - frame_width <= 0 / frame_height <= 0          → ME_E_INVALID_ARG
 *   - confidence_threshold outside [0, 1]           → ME_E_INVALID_ARG
 *   - iou_threshold outside (0, 1]                  → ME_E_INVALID_ARG
 *   - out_bboxes == nullptr                          → ME_E_INVALID_ARG
 */
#pragma once

#include "compose/bbox.hpp"
#include "inference/runtime.hpp"
#include "media_engine/types.h"

#include <string>
#include <vector>

namespace me::compose {

constexpr int kBlazefaceAnchorCount = 896;
constexpr int kBlazefaceRegressorsPerAnchor = 16;

struct BlazefaceDecodeParams {
    /* Input dim (always 128 for the front-camera variant). */
    int   input_width         = 128;
    int   input_height        = 128;
    /* Confidence floor below which detections are dropped. */
    float confidence_threshold = 0.5f;
    /* IoU threshold for greedy NMS suppression. */
    float iou_threshold        = 0.3f;
};

me_status_t decode_blazeface_bboxes(
    const me::inference::Tensor&     regressors,
    const me::inference::Tensor&     classificators,
    int                              frame_width,
    int                              frame_height,
    const BlazefaceDecodeParams&     params,
    std::vector<Bbox>*               out_bboxes,
    std::string*                     err);

}  // namespace me::compose
