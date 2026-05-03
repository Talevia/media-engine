/* decode_blazeface_bboxes impl. See header.
 *
 * Anchor config — canonical MediaPipe SsdAnchorsCalculator
 * configuration for the front-camera (128×128) BlazeFace model:
 *
 *   Layer 0: stride 8  → 16×16 grid, 2 anchors per cell = 512
 *   Layer 1: stride 16 → 8×8   grid, 6 anchors per cell = 384
 *   Total: 896.
 *
 * Anchor centers are placed at cell centers (offset = 0.5).
 * `fixed_anchor_size = true` means all anchors at the same
 * layer share the same width/height — we set both to 1.0 in
 * normalized coords (the regressors learn the actual size via
 * dw / dh).
 */
#include "compose/blazeface_decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace me::compose {

namespace {

struct Anchor {
    float cx;  /* normalized [0, 1] */
    float cy;
};

/* Generate 896 BlazeFace front-camera anchors. Order matches
 * the model's output ordering (layer 0 first, then layer 1). */
std::vector<Anchor> generate_blazeface_anchors() {
    std::vector<Anchor> anchors;
    anchors.reserve(kBlazefaceAnchorCount);

    /* Layer 0: 16×16 grid, 2 anchors per cell. */
    {
        constexpr int grid = 16;
        constexpr int per_cell = 2;
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                const float cx = (static_cast<float>(x) + 0.5f) / grid;
                const float cy = (static_cast<float>(y) + 0.5f) / grid;
                for (int a = 0; a < per_cell; ++a) {
                    anchors.push_back({cx, cy});
                }
            }
        }
    }

    /* Layer 1: 8×8 grid, 6 anchors per cell. */
    {
        constexpr int grid = 8;
        constexpr int per_cell = 6;
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                const float cx = (static_cast<float>(x) + 0.5f) / grid;
                const float cy = (static_cast<float>(y) + 0.5f) / grid;
                for (int a = 0; a < per_cell; ++a) {
                    anchors.push_back({cx, cy});
                }
            }
        }
    }
    return anchors;
}

inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

struct Detection {
    float    confidence;
    float    cx_norm, cy_norm;
    float    w_norm,  h_norm;
};

/* Greedy NMS by descending confidence: drop later detections
 * whose IoU with any earlier kept detection exceeds threshold. */
std::vector<Detection> non_max_suppression(
    std::vector<Detection> detections, float iou_threshold) {
    std::sort(detections.begin(), detections.end(),
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<Detection> kept;
    kept.reserve(detections.size());
    std::vector<bool> suppressed(detections.size(), false);

    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) continue;
        kept.push_back(detections[i]);

        const float ax0 = detections[i].cx_norm - detections[i].w_norm / 2.0f;
        const float ay0 = detections[i].cy_norm - detections[i].h_norm / 2.0f;
        const float ax1 = ax0 + detections[i].w_norm;
        const float ay1 = ay0 + detections[i].h_norm;
        const float a_area = std::max(0.0f, ax1 - ax0) *
                              std::max(0.0f, ay1 - ay0);

        for (std::size_t j = i + 1; j < detections.size(); ++j) {
            if (suppressed[j]) continue;
            const float bx0 = detections[j].cx_norm - detections[j].w_norm / 2.0f;
            const float by0 = detections[j].cy_norm - detections[j].h_norm / 2.0f;
            const float bx1 = bx0 + detections[j].w_norm;
            const float by1 = by0 + detections[j].h_norm;
            const float b_area = std::max(0.0f, bx1 - bx0) *
                                  std::max(0.0f, by1 - by0);

            const float ix0 = std::max(ax0, bx0);
            const float iy0 = std::max(ay0, by0);
            const float ix1 = std::min(ax1, bx1);
            const float iy1 = std::min(ay1, by1);
            const float iw  = std::max(0.0f, ix1 - ix0);
            const float ih  = std::max(0.0f, iy1 - iy0);
            const float inter = iw * ih;
            const float uni   = a_area + b_area - inter;
            const float iou   = (uni > 0.0f) ? inter / uni : 0.0f;

            if (iou > iou_threshold) suppressed[j] = true;
        }
    }
    return kept;
}

}  // namespace

me_status_t decode_blazeface_bboxes(
    const me::inference::Tensor&     regressors,
    const me::inference::Tensor&     classificators,
    int                              frame_width,
    int                              frame_height,
    const BlazefaceDecodeParams&     params,
    std::vector<Bbox>*               out_bboxes,
    std::string*                     err) {
    if (!out_bboxes) return ME_E_INVALID_ARG;
    out_bboxes->clear();

    if (frame_width <= 0 || frame_height <= 0) return ME_E_INVALID_ARG;
    if (!(params.confidence_threshold >= 0.0f &&
          params.confidence_threshold <= 1.0f)) return ME_E_INVALID_ARG;
    if (!(params.iou_threshold > 0.0f &&
          params.iou_threshold <= 1.0f)) return ME_E_INVALID_ARG;
    if (params.input_width <= 0 || params.input_height <= 0) {
        return ME_E_INVALID_ARG;
    }

    if (regressors.dtype != me::inference::Dtype::Float32 ||
        classificators.dtype != me::inference::Dtype::Float32) {
        if (err) *err = "decode_blazeface_bboxes: expected Float32 tensors";
        return ME_E_INVALID_ARG;
    }

    /* regressors: {1, 896, 16}. */
    if (regressors.shape.size() != 3 ||
        regressors.shape[0] != 1 ||
        regressors.shape[1] != kBlazefaceAnchorCount ||
        regressors.shape[2] != kBlazefaceRegressorsPerAnchor) {
        if (err) *err = "decode_blazeface_bboxes: regressors shape mismatch "
                        "(expected {1, 896, 16})";
        return ME_E_INVALID_ARG;
    }
    if (regressors.bytes.size() != static_cast<std::size_t>(
            kBlazefaceAnchorCount * kBlazefaceRegressorsPerAnchor) * 4) {
        if (err) *err = "decode_blazeface_bboxes: regressors bytes mismatch";
        return ME_E_INVALID_ARG;
    }

    /* classificators: {1, 896, 1} or {1, 896}. */
    bool cls_ok = false;
    if (classificators.shape.size() == 3 &&
        classificators.shape[0] == 1 &&
        classificators.shape[1] == kBlazefaceAnchorCount &&
        classificators.shape[2] == 1) {
        cls_ok = true;
    } else if (classificators.shape.size() == 2 &&
               classificators.shape[0] == 1 &&
               classificators.shape[1] == kBlazefaceAnchorCount) {
        cls_ok = true;
    }
    if (!cls_ok) {
        if (err) *err = "decode_blazeface_bboxes: classificators shape mismatch "
                        "(expected {1, 896, 1} or {1, 896})";
        return ME_E_INVALID_ARG;
    }
    if (classificators.bytes.size() !=
            static_cast<std::size_t>(kBlazefaceAnchorCount) * 4) {
        if (err) *err = "decode_blazeface_bboxes: classificators bytes mismatch";
        return ME_E_INVALID_ARG;
    }

    const auto anchors = generate_blazeface_anchors();
    const auto* reg = reinterpret_cast<const float*>(regressors.bytes.data());
    const auto* cls = reinterpret_cast<const float*>(classificators.bytes.data());

    const float in_w_inv = 1.0f / static_cast<float>(params.input_width);
    const float in_h_inv = 1.0f / static_cast<float>(params.input_height);

    /* Step 1+2: per-anchor sigmoid + threshold + bbox decode. */
    std::vector<Detection> filtered;
    filtered.reserve(64);
    for (int i = 0; i < kBlazefaceAnchorCount; ++i) {
        const float conf = sigmoid(cls[i]);
        if (conf < params.confidence_threshold) continue;

        const float* r = reg + static_cast<std::size_t>(i) *
                                kBlazefaceRegressorsPerAnchor;
        const float dx = r[0];
        const float dy = r[1];
        const float dw = r[2];
        const float dh = r[3];

        const float cx_norm = anchors[i].cx + dx * in_w_inv;
        const float cy_norm = anchors[i].cy + dy * in_h_inv;
        const float w_norm  = dw * in_w_inv;
        const float h_norm  = dh * in_h_inv;
        if (w_norm <= 0.0f || h_norm <= 0.0f) continue;

        Detection d{};
        d.confidence = conf;
        d.cx_norm    = cx_norm;
        d.cy_norm    = cy_norm;
        d.w_norm     = w_norm;
        d.h_norm     = h_norm;
        filtered.push_back(d);
    }

    /* Step 3: greedy NMS. */
    const auto kept = non_max_suppression(std::move(filtered),
                                            params.iou_threshold);

    /* Step 4: scale to frame coordinates + emit Bbox. */
    out_bboxes->reserve(kept.size());
    for (const auto& d : kept) {
        const float x0_n = clamp_f(d.cx_norm - d.w_norm / 2.0f, 0.0f, 1.0f);
        const float y0_n = clamp_f(d.cy_norm - d.h_norm / 2.0f, 0.0f, 1.0f);
        const float x1_n = clamp_f(d.cx_norm + d.w_norm / 2.0f, 0.0f, 1.0f);
        const float y1_n = clamp_f(d.cy_norm + d.h_norm / 2.0f, 0.0f, 1.0f);

        Bbox b;
        b.x0 = clamp_int(static_cast<int>(std::floor(x0_n * frame_width  + 0.5f)),
                          0, frame_width);
        b.y0 = clamp_int(static_cast<int>(std::floor(y0_n * frame_height + 0.5f)),
                          0, frame_height);
        b.x1 = clamp_int(static_cast<int>(std::floor(x1_n * frame_width  + 0.5f)),
                          0, frame_width);
        b.y1 = clamp_int(static_cast<int>(std::floor(y1_n * frame_height + 0.5f)),
                          0, frame_height);
        if (!b.empty()) out_bboxes->push_back(b);
    }
    return ME_OK;
}

}  // namespace me::compose
