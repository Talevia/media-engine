/* `me::compose::resolve_mask_alpha_from_file` — read a per-frame
 * alpha-mask sequence from a JSON fixture file at a specific
 * timeline frame.
 *
 * M11 `body-alpha-key-mask-resolver-impl`. Sibling of
 * `landmark_resolver` but for portrait-segmentation masks
 * (Asset.kind == AssetKind::Mask, cycle 28 IR work). The
 * body_alpha_key kernel (`src/compose/body_alpha_key_kernel.cpp`)
 * consumes a per-frame alpha buffer (8-bit alpha per pixel,
 * width × height); this TU is the file-fixture / batch-pipeline
 * resolver. The runtime-driven variant (running portrait-
 * segmentation through `me::inference::Runtime`) is a future
 * cycle once the runtime call sites land.
 *
 * JSON file shape:
 *
 *   {
 *     "frames": [
 *       {
 *         "t": { "num": 0, "den": 30 },
 *         "width": 64,
 *         "height": 64,
 *         "alphaB64": "<base64 of 64*64 raw bytes>"
 *       },
 *       { "t": {...}, "width": ..., "height": ..., "alphaB64": "..." }
 *     ]
 *   }
 *
 * Closest-frame selection logic mirrors `landmark_resolver` —
 * linear scan with rational-arithmetic distance, ties resolve to
 * first in document order.
 *
 * Format choice. Base64 keeps mask sequences inline in JSON
 * (no separate binary fixtures to track), which keeps the
 * fixture-driven test rig self-contained. `av_base64_decode`
 * (libavutil) handles the decode — already in the link graph;
 * no new dependency. For larger production masks (1080p+),
 * the runtime-driven resolver streams alpha bytes directly
 * from inference output without going through JSON / base64
 * — that's the production fast path.
 *
 * Determinism. JSON parse + base64 decode is deterministic.
 * Closest-frame selection uses rational integer compares (no
 * float).
 */
#pragma once

#include "media_engine/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct me_engine;

namespace me::compose {

/* Resolve the alpha mask at `time` from the JSON fixture at
 * `file_uri`. URI accepts the same shapes as
 * `decode_sticker_to_rgba8` / `resolve_landmark_bboxes_from_file`
 * (file://, absolute path, relative path).
 *
 * Empty `frames` array → ME_OK with `*out_width = 0`,
 * `*out_height = 0`, `out_alpha->clear()` (legitimate "no mask
 * available" representation).
 *
 * Return codes:
 *   - ME_OK              — `*out` populated.
 *   - ME_E_INVALID_ARG   — null pointer args / empty URI.
 *   - ME_E_UNSUPPORTED   — URI scheme not supported.
 *   - ME_E_IO            — file open / read failure.
 *   - ME_E_PARSE         — JSON parse failure / required field
 *                          missing / base64 decode failure /
 *                          alpha-byte-count vs (width*height)
 *                          mismatch.
 */
me_status_t resolve_mask_alpha_from_file(
    std::string_view           file_uri,
    me_rational_t              time,
    int*                       out_width,
    int*                       out_height,
    std::vector<std::uint8_t>* out_alpha,
    std::string*               err);

#ifdef ME_HAS_INFERENCE

/* Runtime-mode mask resolver — sibling of
 * `resolve_landmark_bboxes_runtime`, drives a portrait-
 * segmentation Runtime through `make_runtime_for_model` +
 * `run_cached`. M11 `mask-resolver-runtime-mode-impl`
 * (cycle 52). Production caller of run_cached for
 * body_alpha_key's segmentation path; once paired with
 * `selfie-segmentation-ship-path-test` (P1) it covers M11
 * §139's second ship-path model end-to-end.
 *
 * URI shape: `model:<id>/<version>/<quantization>` (e.g.
 * `model:selfie_seg/v3/int8`). Same parser as the landmark
 * sibling. Any other URI shape → ME_E_INVALID_ARG.
 *
 * Skeleton decode. The output-tensor → alpha-plane decode is
 * model-specific (SelfieSegmentation produces a 1×H×W float
 * mask in [0, 1] that needs a sigmoid + uint8 quantize +
 * upscale to frame dims). The decode lives in
 * `selfie-segmentation-mask-decode-impl` follow-up. This
 * skeleton:
 *
 *   1. Parses the URI into the model identity tuple.
 *   2. Calls `make_runtime_for_model` (license whitelist +
 *      content_hash gates).
 *   3. Frame preprocessing via `prepare_selfie_segmentation_input`
 *      (resize + planar conversion + [0, 1] normalize). When
 *      `frame_rgba` is NULL, falls back to a synthetic zero-
 *      filled tensor of the documented shape so the test
 *      callers that don't have real pixels still drive the wire
 *      to Step 4.
 *   4. Calls `run_cached` (engine AssetCache cache key
 *      includes the input bytes).
 *   5. Returns ME_E_UNSUPPORTED on the decode step with diag
 *      naming the follow-up bullet.
 *
 * The first 4 steps ARE the production wire. Returns:
 *   - ME_E_UNSUPPORTED  — decode step (documented stub).
 *   - ME_E_INVALID_ARG  — engine NULL, URI malformed, or
 *                          non-NULL frame_rgba paired with
 *                          invalid frame_width/frame_height/stride.
 *   - propagated factory / run_cached errors. */
me_status_t resolve_mask_alpha_runtime(
    me_engine*                 engine,
    std::string_view           model_uri,
    me_rational_t              frame_t,
    int                        frame_width,
    int                        frame_height,
    const std::uint8_t*        frame_rgba,
    std::size_t                frame_stride_bytes,
    int*                       out_mask_width,
    int*                       out_mask_height,
    std::vector<std::uint8_t>* out_alpha,
    std::string*               err);

#endif /* ME_HAS_INFERENCE */

}  // namespace me::compose
