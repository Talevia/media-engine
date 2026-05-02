/*
 * `me::inference::make_runtime_for_model` — production entry
 * point that turns a model identity tuple into a license-
 * validated, content-hash-checked, platform-appropriate
 * `Runtime*` ready to feed to `me::inference::run_cached`.
 *
 * M11 production-call-site bullet
 * `inference-runtime-factory-impl`. Glues two earlier-cycle
 * pieces:
 *
 *   - `me::inference::load_model_blob` (cycle that landed M11
 *     §138's license whitelist + content_hash gate) — fetches +
 *     validates the model bytes via the engine's host-supplied
 *     fetcher.
 *   - `OnnxRuntime::create` / `CoreMlRuntime::create` (cycle 45
 *     skeletons) — constructs the runtime impl from validated
 *     bytes.
 *
 * Without this factory, production effect stages have no
 * canonical path from a `(model_id, model_version,
 * quantization)` triple to a callable Runtime. The factory IS
 * the canonical path. Effect stages call this once per first
 * use of a model; subsequent calls return the cached Runtime
 * instance via the engine's `loaded_runtimes` map.
 *
 * Runtime selection:
 *
 *   - macOS + ME_HAS_COREML: try CoreML first (HW path).
 *     Fall back to ONNX on CoreML construction failure if
 *     ME_HAS_ONNX_RUNTIME is also compiled in.
 *   - Other platforms: ONNX-only when ME_HAS_ONNX_RUNTIME, else
 *     ME_E_UNSUPPORTED.
 *   - Both compile-flags off: ME_E_UNSUPPORTED ("engine built
 *     without inference runtime").
 *
 * Caching contract:
 *
 *   - First call for `(model_id, version, quantization)`:
 *     loads bytes via `load_model_blob`, constructs runtime,
 *     stores in `engine->loaded_runtimes`, returns the borrowed
 *     pointer via *out_runtime.
 *   - Subsequent calls: returns the same `*out_runtime` without
 *     re-constructing.
 *
 * Lifetime:
 *
 *   - The engine owns the Runtime via `loaded_runtimes`'s
 *     `unique_ptr`. *out_runtime is a borrow that's valid until
 *     `me_engine_destroy` OR `clear_loaded_runtimes`.
 *   - Callers MUST NOT delete *out_runtime.
 */
#pragma once

#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE

#include <string>

struct me_engine;

namespace me::inference {

class Runtime;

/* Make-or-fetch the cached Runtime for the given model identity.
 *
 * Return codes:
 *   - ME_OK              — *out_runtime populated; cache hit or
 *                           fresh construction.
 *   - ME_E_INVALID_ARG   — engine NULL, identifiers NULL/empty,
 *                           or out_runtime NULL.
 *   - ME_E_NOT_FOUND     — host fetcher rejected the load
 *                           (propagated from load_model_blob).
 *   - ME_E_UNSUPPORTED   — license whitelist / content_hash
 *                           rejection (load_model_blob), OR no
 *                           runtime impl compiled into this
 *                           build (neither CoreML nor ONNX).
 *   - ME_E_INTERNAL      — runtime impl's `create()` rejected
 *                           the validated bytes (junk model, etc).
 *
 * The caller-supplied `err` (optional) receives a host-displayable
 * diagnostic on non-OK return. *out_runtime is a borrowed
 * pointer; the engine owns the underlying Runtime. */
me_status_t make_runtime_for_model(me_engine*    engine,
                                    const char*  model_id,
                                    const char*  model_version,
                                    const char*  quantization,
                                    Runtime**    out_runtime,
                                    std::string* err);

/* Drop the engine's cache of constructed Runtime instances
 * (counterpart to `clear_loaded_models`). Primarily useful for
 * tests that need to re-construct the same identity with a
 * different fetcher response or runtime mock. Production code
 * shouldn't need this. */
void clear_loaded_runtimes(me_engine* engine);

}  // namespace me::inference

#endif /* ME_HAS_INFERENCE */
