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

struct me_engine;

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

#ifdef ME_HAS_INFERENCE

/* Runtime-mode landmark resolver — drives a real
 * `me::inference::Runtime` through `me::inference::run_cached`
 * to produce per-frame bboxes. M11
 * `landmark-resolver-runtime-mode-impl` (cycle 51) — first
 * production caller of `run_cached` so M11 §137 (asset cache
 * flowing through production) + §138 (license enforcement in
 * production) become tickable.
 *
 * URI shape: `model:<id>/<version>/<quantization>` (e.g.
 * `model:blazeface/v1/fp32`). The triple is parsed into the
 * `make_runtime_for_model` identity tuple. Any other URI shape
 * → ME_E_INVALID_ARG (callers should dispatch on URI scheme to
 * pick the file-mode resolver vs this runtime-mode resolver).
 *
 * Skeleton decode. The output-tensor → bbox decode is
 * model-specific (BlazeFace's anchor regression + NMS, etc.)
 * and lives in a follow-up bullet
 * `blazeface-anchor-decode-impl`. This skeleton:
 *
 *   1. Parses the URI into the model identity tuple.
 *   2. Calls `make_runtime_for_model` to get a Runtime* (this
 *      exercises load_model_blob + license whitelist +
 *      content_hash + the engine's loaded_runtimes cache).
 *   3. Builds a synthetic 128×128×3 NCHW float32 input tensor
 *      (BlazeFace's documented shape) — actual frame
 *      preprocessing (resize + planar conversion + normalize)
 *      is the `landmark-resolver-input-preprocess-impl`
 *      follow-up.
 *   4. Calls `run_cached` (this exercises the AssetCache + cache
 *      key + Runtime::run path).
 *   5. Returns ME_E_UNSUPPORTED on the decode step with a diag
 *      naming the follow-up bullet. *out is left empty.
 *
 * The first 4 steps ARE the production wire: every call
 * exercises the §137 cache key + §138 license gate. The decode
 * stub returning ME_E_UNSUPPORTED is the
 * `blazeface-anchor-decode-impl` follow-up's responsibility.
 *
 * Return codes:
 *   - ME_E_UNSUPPORTED  — decode step (the documented stub
 *                         until follow-up).
 *   - ME_E_INVALID_ARG  — engine NULL, URI doesn't match the
 *                         `model:<id>/<ver>/<quant>` shape.
 *   - ME_E_NOT_FOUND    — fetcher non-OK (propagated from
 *                         make_runtime_for_model).
 *   - propagated factory errors (license / hash / no backend). */
me_status_t resolve_landmark_bboxes_runtime(
    me_engine*         engine,
    std::string_view   model_uri,
    me_rational_t      frame_t,
    int                frame_width,
    int                frame_height,
    std::vector<Bbox>* out,
    std::string*       err);

#endif /* ME_HAS_INFERENCE */

}  // namespace me::compose
