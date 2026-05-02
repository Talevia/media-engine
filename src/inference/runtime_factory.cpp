/* runtime_factory impl. See header for the contract.
 *
 * Threading discipline: the engine's `loaded_runtimes_mu`
 * mutex is held only during cache lookup + insert. The
 * potentially-slow runtime construction (`OnnxRuntime::create`
 * may parse Ort::SessionOptions; `CoreMlRuntime::create` may
 * write a temp file + dispatch to MLModel) happens with the
 * mutex DROPPED, then a second lock acquires to insert. Two
 * concurrent calls for the same model identity may both
 * construct a Runtime; the second's construction is dropped at
 * insert time (the first one wins; the loser's unique_ptr
 * goes out of scope cleanly).
 */
#ifdef ME_HAS_INFERENCE

#include "inference/runtime_factory.hpp"

#include "core/engine_impl.hpp"
#include "inference/model_loader.hpp"
#include "inference/runtime.hpp"

#ifdef ME_HAS_ONNX_RUNTIME
#include "inference/onnx_runtime.hpp"
#endif

#ifdef __APPLE__
#include "inference/coreml_runtime.hpp"
#endif

#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

namespace me::inference {

namespace {

/* Construct the platform-appropriate Runtime impl from validated
 * bytes. Selection order:
 *   1. macOS: CoreML first (HW path).
 *   2. ONNX runtime second (cross-platform CPU FP32 reference).
 *   3. Both unavailable → ME_E_UNSUPPORTED.
 *
 * On CoreML construction failure WITH ONNX available, fall through
 * to ONNX. This gives macOS hosts the best-effort HW path without
 * crashing on a model CoreML can't handle (e.g. ops the runtime
 * doesn't support). The fallback's diagnostic is appended so the
 * host sees both attempts. */
std::unique_ptr<Runtime> construct_runtime(const std::uint8_t* bytes,
                                            std::size_t         size,
                                            std::string*        err) {
#ifdef __APPLE__
    {
        std::string coreml_err;
        auto coreml = CoreMlRuntime::create(bytes, size, &coreml_err);
        if (coreml) return coreml;
#ifdef ME_HAS_ONNX_RUNTIME
        std::string onnx_err;
        auto onnx = OnnxRuntime::create(bytes, size, &onnx_err);
        if (onnx) {
            if (err) {
                /* Surface BOTH errors so the host sees CoreML's
                 * complaint + the fact that ONNX worked as a
                 * fallback. Useful for triaging "why did this
                 * model not get the HW path". */
                *err = "CoreMlRuntime::create failed (" + coreml_err +
                       "); falling back to OnnxRuntime";
            }
            return onnx;
        }
        if (err) *err = "CoreMlRuntime::create failed (" + coreml_err +
                        "); OnnxRuntime::create also failed (" + onnx_err + ")";
#else
        if (err) *err = "CoreMlRuntime::create failed (" + coreml_err +
                        "); ONNX runtime not compiled in (build with ME_WITH_INFERENCE + ONNX)";
#endif
        return nullptr;
    }
#elif defined(ME_HAS_ONNX_RUNTIME)
    {
        std::string onnx_err;
        auto onnx = OnnxRuntime::create(bytes, size, &onnx_err);
        if (onnx) return onnx;
        if (err) *err = "OnnxRuntime::create failed: " + onnx_err;
        return nullptr;
    }
#else
    (void)bytes;
    (void)size;
    if (err) *err = "make_runtime_for_model: no inference runtime impl "
                    "compiled in (need __APPLE__/CoreML OR ME_HAS_ONNX_RUNTIME)";
    /* LEGIT: this is the build-flag-gated rejection — the engine
     * was compiled without any concrete Runtime backend. The
     * documented diagnostic tells the host how to enable one. */
    return nullptr;
#endif
}

}  // namespace

me_status_t make_runtime_for_model(me_engine*    engine,
                                    const char*  model_id,
                                    const char*  model_version,
                                    const char*  quantization,
                                    Runtime**    out_runtime,
                                    std::string* err) {
    if (!engine || !out_runtime) return ME_E_INVALID_ARG;
    if (!model_id      || model_id[0]      == '\0') {
        if (err) *err = "make_runtime_for_model: model_id is NULL or empty";
        return ME_E_INVALID_ARG;
    }
    if (!model_version || model_version[0] == '\0') {
        if (err) *err = "make_runtime_for_model: model_version is NULL or empty";
        return ME_E_INVALID_ARG;
    }
    /* quantization may be empty (some models have no quantization
     * variant); pass NULL through as "" to match the load_model_blob
     * + cache key convention. */
    const char* quant_arg = (quantization && quantization[0]) ? quantization : "";

    const auto key = std::make_tuple(std::string{model_id},
                                      std::string{model_version},
                                      std::string{quant_arg});

    /* Phase 1: cache lookup. If we already have a Runtime for
     * this identity, return the borrowed pointer. */
    {
        std::lock_guard<std::mutex> lk(engine->loaded_runtimes_mu);
        auto it = engine->loaded_runtimes.find(key);
        if (it != engine->loaded_runtimes.end()) {
            *out_runtime = it->second.get();
            return ME_OK;
        }
    }

    /* Phase 2: load + validate the model bytes via load_model_blob.
     * This honors the engine's host fetcher + license whitelist +
     * content_hash gate. Cached on the engine via load_model_blob's
     * own loaded_models cache, so a subsequent factory-miss for
     * the same identity (e.g. after clear_loaded_runtimes) won't
     * re-fetch the bytes either. */
    LoadedModel loaded;
    me_status_t s = load_model_blob(engine, model_id, model_version,
                                     quant_arg, &loaded);
    if (s != ME_OK) {
        /* Propagate err message from the model loader (it sets
         * engine->last_error already; copy it through to the
         * caller's err out-param). */
        if (err) {
            const char* loader_err = me::detail::get_error(engine);
            if (loader_err && loader_err[0] != '\0') {
                *err = std::string{"make_runtime_for_model: "} + loader_err;
            } else {
                *err = "make_runtime_for_model: load_model_blob failed";
            }
        }
        return s;
    }

    /* Phase 3: construct the platform-appropriate Runtime impl. */
    std::string ctor_err;
    std::unique_ptr<Runtime> rt = construct_runtime(
        loaded.bytes.data(), loaded.bytes.size(), &ctor_err);
    if (!rt) {
        if (err) *err = ctor_err;
        return ME_E_INTERNAL;
    }

    /* Phase 4: insert into cache. Two concurrent constructions for
     * the same identity may race here; the first inserter wins,
     * the loser's unique_ptr destructs cleanly when this function
     * returns. */
    Runtime* borrowed = nullptr;
    {
        std::lock_guard<std::mutex> lk(engine->loaded_runtimes_mu);
        auto [it, inserted] = engine->loaded_runtimes.try_emplace(
            key, std::move(rt));
        borrowed = it->second.get();
    }

    *out_runtime = borrowed;
    return ME_OK;
}

void clear_loaded_runtimes(me_engine* engine) {
    if (!engine) return;
    std::lock_guard<std::mutex> lk(engine->loaded_runtimes_mu);
    engine->loaded_runtimes.clear();
}

}  // namespace me::inference

#endif /* ME_HAS_INFERENCE */
