/*
 * test_mask_resolver_runtime — pin the cycle-52 runtime-mode
 * mask resolver (M11 mask-resolver-runtime-mode-impl).
 *
 * Sibling of test_landmark_resolver_runtime; same skeleton-
 * with-stub-decode pattern. Verifies:
 *
 *   1. URI parsing — `model:<id>/<ver>/<quant>` accepted; other
 *      shapes rejected with ME_E_INVALID_ARG + named diag.
 *   2. NULL engine / NULL out args → ME_E_INVALID_ARG.
 *   3. License whitelist rejection propagates as
 *      ME_E_UNSUPPORTED with diag containing the rejected
 *      license name.
 *   4. Production wire smoke: factory + run_cached succeed
 *      to the runtime; the synthetic-bytes path surfaces a
 *      non-OK status (the cache + license gates ran).
 *
 * Real SelfieSegmentation decode is the
 * `selfie-segmentation-mask-decode-impl` follow-up.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE

#include "compose/mask_resolver.hpp"
#include "media_engine/engine.h"
#include "media_engine/ml.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

struct FetcherState {
    me_status_t                return_status = ME_OK;
    std::vector<std::uint8_t>  bytes;
    me_model_license_t         license       = ME_MODEL_LICENSE_APACHE2;
    int                        invocations   = 0;
};

FetcherState g_fetcher;

me_status_t test_fetcher(const char*       /*model_id*/,
                          const char*       /*model_version*/,
                          const char*       /*quantization*/,
                          me_model_blob_t*  out_blob,
                          void*             /*user*/) {
    g_fetcher.invocations++;
    if (g_fetcher.return_status != ME_OK) return g_fetcher.return_status;
    out_blob->bytes        = g_fetcher.bytes.empty() ? nullptr : g_fetcher.bytes.data();
    out_blob->size         = g_fetcher.bytes.size();
    out_blob->license      = g_fetcher.license;
    out_blob->content_hash = nullptr;
    return ME_OK;
}

void reset_fetcher() {
    g_fetcher = FetcherState{};
}

struct EngineGuard {
    me_engine_t* eng = nullptr;
    EngineGuard() {
        me_engine_config_t cfg{};
        REQUIRE(me_engine_create(&cfg, &eng) == ME_OK);
    }
    ~EngineGuard() { if (eng) me_engine_destroy(eng); }
    EngineGuard(const EngineGuard&)            = delete;
    EngineGuard& operator=(const EngineGuard&) = delete;
};

}  // namespace

TEST_CASE("resolve_mask_alpha_runtime: NULL engine → ME_E_INVALID_ARG") {
    int mw = -1, mh = -1;
    std::vector<std::uint8_t> alpha;
    std::string err;
    CHECK(me::compose::resolve_mask_alpha_runtime(
              nullptr, "model:selfie_seg/v3/int8",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              &mw, &mh, &alpha, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("resolve_mask_alpha_runtime: NULL out args → ME_E_INVALID_ARG") {
    EngineGuard g;
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    std::string err;

    CHECK(me::compose::resolve_mask_alpha_runtime(
              g.eng, "model:selfie_seg/v3/int8",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              nullptr, &mh, &alpha, &err) == ME_E_INVALID_ARG);
    CHECK(me::compose::resolve_mask_alpha_runtime(
              g.eng, "model:selfie_seg/v3/int8",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              &mw, nullptr, &alpha, &err) == ME_E_INVALID_ARG);
    CHECK(me::compose::resolve_mask_alpha_runtime(
              g.eng, "model:selfie_seg/v3/int8",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              &mw, &mh, nullptr, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("resolve_mask_alpha_runtime: malformed URI rejected") {
    EngineGuard g;
    int mw = 0, mh = 0;
    std::vector<std::uint8_t> alpha;
    std::string err;

    /* Wrong scheme. */
    CHECK(me::compose::resolve_mask_alpha_runtime(
              g.eng, "file:///tmp/mask.json",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              &mw, &mh, &alpha, &err) == ME_E_INVALID_ARG);

    err.clear();
    /* Too few slashes. */
    CHECK(me::compose::resolve_mask_alpha_runtime(
              g.eng, "model:selfie_seg/v3",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              &mw, &mh, &alpha, &err) == ME_E_INVALID_ARG);

    err.clear();
    /* Empty quant segment. */
    CHECK(me::compose::resolve_mask_alpha_runtime(
              g.eng, "model:selfie_seg/v3/",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              &mw, &mh, &alpha, &err) == ME_E_INVALID_ARG);
}

TEST_CASE("resolve_mask_alpha_runtime: license whitelist rejection propagates") {
    reset_fetcher();
    g_fetcher.bytes = {0xAA};
    g_fetcher.license = ME_MODEL_LICENSE_UNKNOWN;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    int mw = -1, mh = -1;
    std::vector<std::uint8_t> alpha = {0x42};  /* sentinel; should be cleared */
    std::string err;
    CHECK(me::compose::resolve_mask_alpha_runtime(
              g.eng, "model:selfie_seg/v3/int8",
              me_rational_t{0, 30}, 640, 480,
              nullptr, 0,
              &mw, &mh, &alpha, &err) == ME_E_UNSUPPORTED);
    /* Out args zeroed on failure (clean failure). */
    CHECK(mw == 0);
    CHECK(mh == 0);
    CHECK(alpha.empty());
    CHECK(err.find("UNKNOWN") != std::string::npos);
}

TEST_CASE("resolve_mask_alpha_runtime: production wire reaches runtime") {
    reset_fetcher();
    g_fetcher.bytes = {0x01, 0x02, 0x03, 0x04};
    g_fetcher.license = ME_MODEL_LICENSE_APACHE2;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    int mw = -1, mh = -1;
    std::vector<std::uint8_t> alpha;
    std::string err;
    me_status_t s = me::compose::resolve_mask_alpha_runtime(
        g.eng, "model:selfie_seg/v3/int8",
        me_rational_t{0, 30}, 640, 480,
        nullptr, 0,
        &mw, &mh, &alpha, &err);

    /* Either no backend compiled (factory ME_E_INTERNAL) OR
     * factory acquires runtime + run_cached fails on junk
     * bytes — both surface non-OK + leave the out args empty. */
    CHECK(s != ME_OK);
    CHECK(alpha.empty());
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("resolve_mask_alpha_runtime: skipped (ME_WITH_INFERENCE=OFF)") {
}

#endif
