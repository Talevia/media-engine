/*
 * ML inference public API — host-supplied model weight loading
 * + license validation hook.
 *
 * Gated by `ME_HAS_INFERENCE`: this whole header is empty when the
 * engine is built with `-DME_WITH_INFERENCE=OFF` (the default). Hosts
 * compiling against an OFF-build of the engine cannot accidentally
 * call inference APIs that have no implementation. ON-builds expose
 * the symbols below + their implementations in src/api/ml.cpp.
 *
 * Usage shape (M11 only; pre-cycle-29 the engine has no inference
 * runtime, so registered fetchers are stored but never invoked —
 * the surface lands ahead of the runtime so M11 work can wire
 * against a stable API):
 *
 *   1. Host calls me_engine_create with default config.
 *   2. Host calls me_engine_set_model_fetcher(engine, my_fetcher,
 *      my_user) once after create. The callback persists for the
 *      engine's lifetime.
 *   3. When an inference-driven effect lands and references a model
 *      via {model_id, model_version, quantization}, the engine's
 *      internal runtime calls the fetcher with those identifiers;
 *      the fetcher returns the model's bytes + license + content
 *      hash through `me_model_blob_t`.
 *   4. Engine validates `me_model_blob_t.license` against the
 *      whitelist (Apache 2.0 / MIT / BSD / CC-BY); rejects with
 *      ME_E_UNSUPPORTED if not listed (license string surfaced via
 *      me_engine_last_error).
 *   5. Engine validates `me_model_blob_t.content_hash` (when set)
 *      against the timeline's MlAssetMetadata-recorded hash if any;
 *      rejects on mismatch.
 *   6. Bytes flow into the inference runtime (CoreML / ONNX-runtime
 *      per ME_WITH_INFERENCE wiring).
 *
 * License whitelist rationale: VISION §3.4 LGPL ship-clean line
 * forbids GPL / non-commercial weights from polluting the engine.
 * Models that AREN'T Apache/MIT/BSD/CC-BY would need explicit
 * legal review and a separate cycle to expand the whitelist.
 *
 * Pre-cycle-29 status: stub-but-functional — the registration path
 * stores the callback verbatim. The fetcher is never invoked by
 * any current code path. Follow-up M11 cycles wire the inference
 * runtime that consults the stored fetcher.
 *
 * Threading: the callback may be invoked from any engine-owned
 * thread (decode worker, scheduler thread, etc.). Host implementations
 * must be thread-safe. Setting / clearing the callback is mutex-
 * guarded by the engine and may be called from any thread.
 */
#ifndef MEDIA_ENGINE_ML_H
#define MEDIA_ENGINE_ML_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ME_HAS_INFERENCE

/* Model license axis — matches the whitelist anchored in
 * VISION §3.4. Engine rejects anything outside this enum
 * (any host that returns a non-listed value through the
 * fetcher gets ME_E_UNSUPPORTED back from the inference
 * load path). New license entries require a corresponding
 * ARCHITECTURE.md whitelist update before we accept them.
 *
 * Stable enum values — append-only ABI evolution per
 * `docs/ARCHITECTURE.md` ABI principles. */
typedef enum me_model_license {
    ME_MODEL_LICENSE_UNKNOWN  = 0,    /* Always rejected. */
    ME_MODEL_LICENSE_APACHE2  = 1,    /* Apache License 2.0. */
    ME_MODEL_LICENSE_MIT      = 2,    /* MIT. */
    ME_MODEL_LICENSE_BSD      = 3,    /* BSD-2 / BSD-3. */
    ME_MODEL_LICENSE_CC_BY    = 4     /* CC-BY 4.0. */
} me_model_license_t;

/* Result struct populated by the host's fetcher callback. POD —
 * passes across the C ABI. The host owns `bytes` for the duration
 * of the callback frame; the engine memcpy's its own copy before
 * the callback returns. `content_hash` is optional (NULL when
 * unknown); when non-NULL it MUST be a NUL-terminated lowercase
 * 64-char SHA256 hex string (matches MlAssetMetadata::content_hash
 * format from src/timeline/timeline_impl.hpp). */
typedef struct me_model_blob {
    const uint8_t*       bytes;
    size_t               size;
    me_model_license_t   license;
    const char*          content_hash;   /* NULL or 64-char SHA256 hex; NUL-terminated */
} me_model_blob_t;

/* Host fetcher callback. The engine calls this when an inference-
 * driven effect references a model that hasn't been loaded yet.
 *
 *   model_id        — Asset.ml_metadata->model_id          (NUL-terminated)
 *   model_version   — Asset.ml_metadata->model_version     (NUL-terminated)
 *   quantization    — Asset.ml_metadata->quantization      (NUL-terminated)
 *   out_blob        — host populates the result; engine frees
 *                     after copying contents.
 *   user            — opaque pointer passed at registration.
 *
 * Return ME_OK on success. Any non-OK status surfaces back to the
 * inference-load caller as ME_E_NOT_FOUND with the host-provided
 * me_engine_last_error message (engine populates that side-channel
 * when the fetcher returns non-OK). */
typedef me_status_t (*me_model_fetcher_t)(
    const char*       model_id,
    const char*       model_version,
    const char*       quantization,
    me_model_blob_t*  out_blob,
    void*             user);

/* Register (or clear, when cb=NULL) the model fetcher. The callback
 * persists for the engine's lifetime; subsequent calls overwrite.
 * `user` is borrowed by the engine (not freed) — host is responsible
 * for keeping it alive across all possible invocations.
 *
 * Returns ME_OK on success, ME_E_INVALID_ARG when engine is NULL.
 *
 * Thread-safe: may be called from any thread; the callback storage
 * is mutex-guarded. */
ME_API me_status_t me_engine_set_model_fetcher(
    me_engine_t*        engine,
    me_model_fetcher_t  cb,
    void*               user);

#endif /* ME_HAS_INFERENCE */

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_ENGINE_ML_H */
