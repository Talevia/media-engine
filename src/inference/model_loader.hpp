/*
 * Model-blob loader — invokes the engine's host-supplied
 * `me_model_fetcher_t`, validates the resulting blob's license
 * against VISION §3.4's whitelist, optionally verifies the blob's
 * `content_hash` against the bytes (SHA-256), and surfaces an
 * owned byte copy to the caller.
 *
 * M11 exit criterion at `docs/MILESTONES.md:138`:
 *   `engine 校验 content_hash + license 白名单（Apache / MIT / BSD
 *   / CC-BY），non-commercial / GPL / unknown license 拒载`
 *
 * The C-API surface (`me_engine_set_model_fetcher` +
 * `me_model_blob_t` + `me_model_license_t`) is already public via
 * `include/media_engine/ml.h`; this loader is the engine-internal
 * wiring that consumes the registered fetcher and enforces the
 * whitelist before bytes flow into the inference runtime. Inference-
 * driven effects call this through their compose stages; this TU is
 * not exposed to hosts.
 *
 * Threading. The engine's fetcher mutex is held only while the
 * fetcher executes; whitelist + hash validation runs without the
 * lock so concurrent loads of distinct models don't serialize.
 *
 * License whitelist. {APACHE2, MIT, BSD, CC_BY} accepted; UNKNOWN
 * (and any future non-whitelisted enum values) rejected with
 * `ME_E_UNSUPPORTED` and a diagnostic naming the rejected license.
 * Expanding the whitelist requires both an enum entry in
 * `include/media_engine/ml.h` AND an explicit acceptance branch
 * here — the redundant check is the deliberate fail-closed default.
 *
 * Content hash. When the fetcher returns a non-NULL `content_hash`
 * (64-char lowercase SHA-256 hex per the header contract), the
 * loader recomputes SHA-256 over the returned bytes and rejects
 * mismatches with `ME_E_UNSUPPORTED` + a diagnostic showing the
 * declared vs. computed digests. NULL content_hash is treated as
 * "host doesn't claim integrity" and skipped — an explicit "trust
 * the fetcher" path that lets test fixtures + early integrations
 * skip hash bookkeeping.
 */
#pragma once

#include "media_engine/ml.h"
#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE

#include <cstdint>
#include <string>
#include <vector>

struct me_engine;

namespace me::inference {

/* Owned result of a successful `load_model_blob` call. `bytes` is
 * an engine-owned copy of the host's bytes (the fetcher's
 * `me_model_blob_t.bytes` is borrowed only for the callback's
 * duration per `include/media_engine/ml.h` contract). */
struct LoadedModel {
    std::vector<std::uint8_t> bytes;
    me_model_license_t        license = ME_MODEL_LICENSE_UNKNOWN;
    /* Empty when the fetcher returned a NULL content_hash; otherwise
     * the 64-char lowercase hex digest the fetcher claimed (already
     * validated to match `bytes`). */
    std::string               content_hash;
};

/* Invoke the engine's registered model fetcher + run all
 * validations. On success populates *out and returns ME_OK; on
 * failure populates engine's last_error (via me::detail::set_error)
 * and returns one of:
 *
 *   - ME_E_INVALID_ARG    — engine NULL, identifiers NULL/empty, or
 *                            no fetcher registered.
 *   - ME_E_NOT_FOUND      — fetcher returned non-OK status.
 *   - ME_E_UNSUPPORTED    — license not whitelisted, OR content_hash
 *                            was provided but disagreed with the
 *                            SHA-256 of the returned bytes.
 *
 * Identifiers (`model_id`, `model_version`, `quantization`) MUST be
 * NUL-terminated. They're passed through to the fetcher verbatim.
 * `quantization` may be empty (some models have no quantization
 * variant); the others must be non-empty. */
me_status_t load_model_blob(me_engine*          engine,
                            const char*         model_id,
                            const char*         model_version,
                            const char*         quantization,
                            LoadedModel*        out);

/* Pure license-axis check, exported for unit-test coverage that
 * doesn't want to set up a full engine + fetcher. Returns true iff
 * `license` is in the whitelist. */
bool license_is_whitelisted(me_model_license_t license) noexcept;

}  // namespace me::inference

#endif /* ME_HAS_INFERENCE */
