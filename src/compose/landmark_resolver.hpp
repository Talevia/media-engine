/* `me::compose::resolve_landmark_bboxes_from_file` — read pre-
 * computed landmark bboxes from a JSON file at a specific
 * timeline frame.
 *
 * M11 `face-sticker-compose-stage-wiring`. The face_sticker /
 * face_mosaic kernels operate over `std::span<const Bbox>`
 * (`compose/bbox.hpp`); this TU is the upstream resolver for
 * pre-computed landmarks (testing / batch-pipeline path). The
 * runtime-driven resolver — running BlazeFace through
 * `me::inference::Runtime` — is a separate function added once
 * the inference call sites are wired (M11 follow-up
 * `landmark-resolver-runtime-mode-impl`).
 *
 * JSON file shape:
 *
 *   {
 *     "frames": [
 *       {
 *         "t": { "num": 0, "den": 30 },
 *         "bboxes": [
 *           { "x0": 100, "y0": 50, "x1": 300, "y1": 250 },
 *           { "x0": 400, "y0": 80, "x1": 600, "y1": 280 }
 *         ]
 *       },
 *       { "t": { "num": 30, "den": 30 }, "bboxes": [ ... ] }
 *     ]
 *   }
 *
 * The frame closest to `time` is selected (by absolute distance
 * in rational arithmetic). Frames must be sorted by `t`; the
 * resolver doesn't sort but does handle out-of-order via linear
 * scan (acceptable for synthetic fixtures of size ~< 1000 frames;
 * production-scale streams use the runtime resolver instead).
 *
 * Determinism. JSON parse + bbox copy is deterministic. The
 * "closest-frame" selection uses `(num*den2 - num2*den)` integer
 * comparisons — no float, no cross-host SIMD.
 */
#pragma once

#include "compose/bbox.hpp"
#include "media_engine/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace me::compose {

/* Resolve the landmark bboxes at `time` from the JSON fixture
 * at `file_uri`. URI accepts the same shapes as
 * `decode_sticker_to_rgba8` (`file://`, absolute path,
 * relative path).
 *
 * Empty `frames` array → ME_OK with `out->clear()` (legitimate
 * "low-confidence frame" representation per `bbox.hpp`).
 *
 * Return codes:
 *   - ME_OK             — `*out` populated (may be empty).
 *   - ME_E_INVALID_ARG  — null pointer args / empty URI.
 *   - ME_E_UNSUPPORTED  — URI scheme not supported.
 *   - ME_E_IO           — file open / read failure.
 *   - ME_E_PARSE        — JSON parse failure / required field
 *                         missing / bbox shape invalid. */
me_status_t resolve_landmark_bboxes_from_file(
    std::string_view  file_uri,
    me_rational_t     time,
    std::vector<Bbox>* out,
    std::string*      err);

}  // namespace me::compose
