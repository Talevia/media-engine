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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

}  // namespace me::compose
