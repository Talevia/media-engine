/* Model-blob loader impl. See header for the contract.
 *
 * Fetcher-mutex discipline mirrors the existing
 * `me_engine_set_model_fetcher` (`src/api/ml.cpp`) — both grab
 * `engine->model_fetcher_mu` before reading the callback +
 * user pointers. The mutex is released before invoking the
 * callback so a long-running fetcher doesn't block other
 * loaders; the snapshot pattern (cb + user copied under the
 * lock) makes this safe even if a sibling thread concurrently
 * clears the fetcher mid-load.
 */
#ifdef ME_HAS_INFERENCE

#include "inference/model_loader.hpp"

#include "core/engine_impl.hpp"
#include "resource/content_hash.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace me::inference {

namespace {

const char* license_name(me_model_license_t l) noexcept {
    switch (l) {
    case ME_MODEL_LICENSE_UNKNOWN: return "UNKNOWN";
    case ME_MODEL_LICENSE_APACHE2: return "APACHE2";
    case ME_MODEL_LICENSE_MIT:     return "MIT";
    case ME_MODEL_LICENSE_BSD:     return "BSD";
    case ME_MODEL_LICENSE_CC_BY:   return "CC_BY";
    }
    return "INVALID";
}

}  // namespace

bool license_is_whitelisted(me_model_license_t l) noexcept {
    /* Closed enumeration — every accepted license must appear here.
     * Adding a new enum value to `me_model_license_t` without an
     * accompanying branch here means the value falls through to
     * `default: return false` — fail-closed by design. */
    switch (l) {
    case ME_MODEL_LICENSE_APACHE2:
    case ME_MODEL_LICENSE_MIT:
    case ME_MODEL_LICENSE_BSD:
    case ME_MODEL_LICENSE_CC_BY:
        return true;
    case ME_MODEL_LICENSE_UNKNOWN:
        return false;
    }
    return false;
}

me_status_t load_model_blob(me_engine*        engine,
                             const char*       model_id,
                             const char*       model_version,
                             const char*       quantization,
                             LoadedModel*      out) {
    if (!engine || !out) return ME_E_INVALID_ARG;
    if (!model_id      || model_id[0]      == '\0') {
        me::detail::set_error(engine, "load_model_blob: model_id is NULL or empty");
        return ME_E_INVALID_ARG;
    }
    if (!model_version || model_version[0] == '\0') {
        me::detail::set_error(engine, "load_model_blob: model_version is NULL or empty");
        return ME_E_INVALID_ARG;
    }
    /* quantization may legitimately be empty (some models have no
     * quantization variant); pass through verbatim. NULL → use empty
     * string for the fetcher's NUL-terminated arg. */
    const char* quant_arg = (quantization && quantization[0]) ? quantization : "";

    /* Engine cache lookup — if this exact identity already passed
     * validation, return the stored LoadedModel directly without
     * round-tripping through the host fetcher. Cache is keyed on
     * (id, version, quant); cache value is the full validated blob
     * (bytes + license + content_hash). */
    {
        std::lock_guard<std::mutex> lk(engine->loaded_models_mu);
        auto it = engine->loaded_models.find(
            std::make_tuple(std::string{model_id},
                             std::string{model_version},
                             std::string{quant_arg}));
        if (it != engine->loaded_models.end()) {
            *out = it->second;  /* deep copy — caller mutates freely */
            return ME_OK;
        }
    }

    /* Snapshot the fetcher under the lock so the actual call runs
     * without holding the engine's mutex. */
    me_model_fetcher_t cb = nullptr;
    void*              cb_user = nullptr;
    {
        std::lock_guard<std::mutex> lk(engine->model_fetcher_mu);
        cb      = engine->model_fetcher_cb;
        cb_user = engine->model_fetcher_user;
    }
    if (!cb) {
        me::detail::set_error(engine,
            "load_model_blob: no fetcher registered "
            "(call me_engine_set_model_fetcher first)");
        return ME_E_INVALID_ARG;
    }

    me_model_blob_t blob{};
    /* Default-init explicitly so a host that forgets to populate a
     * field doesn't read garbage on rejection paths. */
    blob.bytes        = nullptr;
    blob.size         = 0;
    blob.license      = ME_MODEL_LICENSE_UNKNOWN;
    blob.content_hash = nullptr;

    me_status_t rc = cb(model_id, model_version, quant_arg, &blob, cb_user);
    if (rc != ME_OK) {
        /* Per the public ml.h contract, a non-OK fetcher return
         * surfaces as ME_E_NOT_FOUND. The host's diag (if any)
         * should already be in last_error via the engine, but
         * normalize it to our own diag if empty. */
        const char* host_msg = me::detail::get_error(engine);
        if (!host_msg || host_msg[0] == '\0') {
            me::detail::set_error(engine,
                std::string{"load_model_blob: fetcher returned non-OK ("} +
                std::to_string(static_cast<int>(rc)) +
                ") for model_id='" + model_id + "'");
        }
        return ME_E_NOT_FOUND;
    }

    /* Validate the blob shape before trusting any field. A fetcher
     * that returns OK with NULL bytes is a host bug — fail loud. */
    if (!blob.bytes || blob.size == 0) {
        me::detail::set_error(engine,
            std::string{"load_model_blob: fetcher returned OK but bytes/size is empty for model_id='"} +
            model_id + "'");
        return ME_E_INVALID_ARG;
    }

    /* License whitelist enforcement. VISION §3.4 LGPL-clean line +
     * the public ml.h doc both anchor the {APACHE2, MIT, BSD,
     * CC_BY} set; UNKNOWN (or any future non-whitelisted enum
     * value) is rejected with a diag naming the offending license
     * so hosts can debug without reading source. */
    if (!license_is_whitelisted(blob.license)) {
        me::detail::set_error(engine,
            std::string{"load_model_blob: model_id='"} + model_id +
            "' rejected — license '" + license_name(blob.license) +
            "' not in whitelist {APACHE2, MIT, BSD, CC_BY} (VISION §3.4)");
        /* LEGIT: license-whitelist rejection is the documented gate
         * (M11 exit criterion at docs/MILESTONES.md:138) — not a
         * stub, this IS the desired runtime behavior. */
        return ME_E_UNSUPPORTED;
    }

    /* Content-hash validation — when the fetcher claims a hash, we
     * recompute SHA-256 over the bytes and compare. NULL content_hash
     * = host opts out (test fixtures + early integrations); empty
     * string is also treated as "no claim". */
    std::string declared_hash;
    if (blob.content_hash && blob.content_hash[0] != '\0') {
        declared_hash = blob.content_hash;
        const std::string computed =
            me::resource::sha256_hex(blob.bytes, blob.size);
        if (computed != declared_hash) {
            me::detail::set_error(engine,
                std::string{"load_model_blob: model_id='"} + model_id +
                "' content_hash mismatch — declared '" + declared_hash +
                "' vs. computed '" + computed + "'");
            /* LEGIT: content_hash integrity check is the documented
             * gate (M11 exit criterion at docs/MILESTONES.md:138) —
             * the rejection IS the desired behavior on hash drift. */
            return ME_E_UNSUPPORTED;
        }
    }

    /* All validations passed — copy bytes into engine-owned storage
     * before returning (host owns the source buffer only for the
     * callback's duration per ml.h's "host owns `bytes`" contract). */
    out->bytes.assign(blob.bytes, blob.bytes + blob.size);
    out->license      = blob.license;
    out->content_hash = std::move(declared_hash);

    /* Cache the validated blob under the engine. Subsequent calls
     * for the same identity skip the fetcher round-trip + revalidation. */
    {
        std::lock_guard<std::mutex> lk(engine->loaded_models_mu);
        engine->loaded_models[std::make_tuple(
            std::string{model_id},
            std::string{model_version},
            std::string{quant_arg})] = *out;
    }
    return ME_OK;
}

void clear_loaded_models(me_engine* engine) {
    if (!engine) return;
    std::lock_guard<std::mutex> lk(engine->loaded_models_mu);
    engine->loaded_models.clear();
}

}  // namespace me::inference

#endif /* ME_HAS_INFERENCE */
