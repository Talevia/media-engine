/*
 * test_inference_license_whitelist — covers
 * `me::inference::load_model_blob` (M11 exit criterion at
 * docs/MILESTONES.md:138, license whitelist enforcement).
 *
 * Coverage:
 *   - license_is_whitelisted: APACHE2/MIT/BSD/CC_BY accept;
 *     UNKNOWN reject.
 *   - load_model_blob:
 *       - NULL engine / NULL identifiers → ME_E_INVALID_ARG.
 *       - No fetcher registered → ME_E_INVALID_ARG with diag.
 *       - Fetcher returns non-OK → ME_E_NOT_FOUND with diag.
 *       - Fetcher returns OK + APACHE2 + matching content_hash →
 *         ME_OK + bytes copied.
 *       - Fetcher returns OK + MIT/BSD/CC_BY → ME_OK.
 *       - Fetcher returns OK + UNKNOWN license → ME_E_UNSUPPORTED
 *         + diag names the rejected license.
 *       - Fetcher returns OK + APACHE2 + mismatched content_hash
 *         → ME_E_UNSUPPORTED + diag shows declared/computed pair.
 *       - Fetcher returns OK + APACHE2 + NULL content_hash →
 *         ME_OK (host opts out of integrity check).
 *       - Fetcher returns OK + APACHE2 + empty bytes → ME_E_INVALID_ARG.
 *
 * Gated on ME_HAS_INFERENCE — same shape as the other inference
 * tests. OFF builds compile to a single skipped TEST_CASE.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE

#include "inference/model_loader.hpp"
#include "media_engine/engine.h"
#include "media_engine/ml.h"
#include "resource/content_hash.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* Fixture controls a fetcher's response via static globals so the
 * C-callable callback can read them without capture. doctest runs
 * test cases sequentially (no parallel cases), so global state is
 * safe — each TEST_CASE resets before invoking. */
struct FetcherState {
    me_status_t                return_status   = ME_OK;
    std::vector<std::uint8_t>  bytes;
    me_model_license_t         license         = ME_MODEL_LICENSE_APACHE2;
    bool                       expose_hash     = true;
    std::string                declared_hash;
    int                        invocations     = 0;
    std::string                last_model_id;
    std::string                last_model_ver;
    std::string                last_quantization;
};

FetcherState g_fetcher;

me_status_t test_fetcher(const char*       model_id,
                          const char*       model_version,
                          const char*       quantization,
                          me_model_blob_t*  out_blob,
                          void*             /*user*/) {
    g_fetcher.invocations++;
    g_fetcher.last_model_id     = model_id ? model_id : "";
    g_fetcher.last_model_ver    = model_version ? model_version : "";
    g_fetcher.last_quantization = quantization ? quantization : "";
    if (g_fetcher.return_status != ME_OK) return g_fetcher.return_status;

    out_blob->bytes        = g_fetcher.bytes.empty() ? nullptr : g_fetcher.bytes.data();
    out_blob->size         = g_fetcher.bytes.size();
    out_blob->license      = g_fetcher.license;
    out_blob->content_hash = g_fetcher.expose_hash ? g_fetcher.declared_hash.c_str() : nullptr;
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
};

}  // namespace

TEST_CASE("license_is_whitelisted: accepts APACHE2/MIT/BSD/CC_BY, rejects UNKNOWN") {
    using namespace me::inference;
    CHECK(license_is_whitelisted(ME_MODEL_LICENSE_APACHE2));
    CHECK(license_is_whitelisted(ME_MODEL_LICENSE_MIT));
    CHECK(license_is_whitelisted(ME_MODEL_LICENSE_BSD));
    CHECK(license_is_whitelisted(ME_MODEL_LICENSE_CC_BY));
    CHECK_FALSE(license_is_whitelisted(ME_MODEL_LICENSE_UNKNOWN));
}

TEST_CASE("load_model_blob: NULL engine → ME_E_INVALID_ARG") {
    me::inference::LoadedModel out;
    CHECK(me::inference::load_model_blob(nullptr, "m", "v1", "fp32", &out)
          == ME_E_INVALID_ARG);
}

TEST_CASE("load_model_blob: empty model_id → ME_E_INVALID_ARG with diag") {
    EngineGuard g;
    me::inference::LoadedModel out;
    CHECK(me::inference::load_model_blob(g.eng, "", "v1", "fp32", &out)
          == ME_E_INVALID_ARG);
    const char* err = me_engine_last_error(g.eng);
    CHECK(std::string{err}.find("model_id") != std::string::npos);
}

TEST_CASE("load_model_blob: no fetcher registered → ME_E_INVALID_ARG with diag") {
    EngineGuard g;
    me::inference::LoadedModel out;
    CHECK(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out)
          == ME_E_INVALID_ARG);
    const char* err = me_engine_last_error(g.eng);
    CHECK(std::string{err}.find("no fetcher") != std::string::npos);
}

TEST_CASE("load_model_blob: fetcher returns non-OK → ME_E_NOT_FOUND") {
    reset_fetcher();
    g_fetcher.return_status = ME_E_IO;
    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);
    me::inference::LoadedModel out;
    CHECK(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out)
          == ME_E_NOT_FOUND);
    CHECK(g_fetcher.invocations == 1);
    /* Identifiers were forwarded verbatim. */
    CHECK(g_fetcher.last_model_id == "m");
    CHECK(g_fetcher.last_model_ver == "v1");
    CHECK(g_fetcher.last_quantization == "fp32");
}

TEST_CASE("load_model_blob: APACHE2 + matching content_hash → ME_OK + bytes copied") {
    reset_fetcher();
    g_fetcher.bytes         = {1, 2, 3, 4, 5};
    g_fetcher.license       = ME_MODEL_LICENSE_APACHE2;
    g_fetcher.declared_hash = me::resource::sha256_hex(g_fetcher.bytes.data(),
                                                        g_fetcher.bytes.size());
    g_fetcher.expose_hash   = true;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);
    me::inference::LoadedModel out;
    REQUIRE(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out) == ME_OK);
    CHECK(out.bytes == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
    CHECK(out.license == ME_MODEL_LICENSE_APACHE2);
    CHECK(out.content_hash == g_fetcher.declared_hash);
}

TEST_CASE("load_model_blob: MIT/BSD/CC_BY all accepted") {
    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    for (auto lic : {ME_MODEL_LICENSE_MIT,
                       ME_MODEL_LICENSE_BSD,
                       ME_MODEL_LICENSE_CC_BY}) {
        reset_fetcher();
        g_fetcher.bytes         = {0xAA};
        g_fetcher.license       = lic;
        g_fetcher.expose_hash   = false;  /* skip hash check */

        me::inference::LoadedModel out;
        REQUIRE(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out) == ME_OK);
        CHECK(out.license == lic);
        CHECK(out.bytes.size() == 1);
    }
}

TEST_CASE("load_model_blob: UNKNOWN license → ME_E_UNSUPPORTED + diag") {
    reset_fetcher();
    g_fetcher.bytes       = {0x00};
    g_fetcher.license     = ME_MODEL_LICENSE_UNKNOWN;
    g_fetcher.expose_hash = false;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);
    me::inference::LoadedModel out;
    CHECK(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out)
          == ME_E_UNSUPPORTED);
    const char* err = me_engine_last_error(g.eng);
    CHECK(std::string{err}.find("UNKNOWN") != std::string::npos);
    CHECK(std::string{err}.find("whitelist") != std::string::npos);
}

TEST_CASE("load_model_blob: APACHE2 + mismatched content_hash → ME_E_UNSUPPORTED + diag") {
    reset_fetcher();
    g_fetcher.bytes         = {1, 2, 3};
    g_fetcher.license       = ME_MODEL_LICENSE_APACHE2;
    /* Wrong hash on purpose — 64 zeros. */
    g_fetcher.declared_hash = std::string(64, '0');
    g_fetcher.expose_hash   = true;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);
    me::inference::LoadedModel out;
    CHECK(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out)
          == ME_E_UNSUPPORTED);
    const char* err = me_engine_last_error(g.eng);
    CHECK(std::string{err}.find("content_hash mismatch") != std::string::npos);
    /* The diag must include both declared and computed digests so
     * the host can debug without rerunning. */
    CHECK(std::string{err}.find(g_fetcher.declared_hash) != std::string::npos);
}

TEST_CASE("load_model_blob: APACHE2 + NULL content_hash → ME_OK (host opts out)") {
    reset_fetcher();
    g_fetcher.bytes       = {0xFF, 0xEE};
    g_fetcher.license     = ME_MODEL_LICENSE_APACHE2;
    g_fetcher.expose_hash = false;  /* makes content_hash NULL in callback */

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);
    me::inference::LoadedModel out;
    REQUIRE(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out) == ME_OK);
    CHECK(out.content_hash.empty());
    CHECK(out.bytes.size() == 2);
}

TEST_CASE("load_model_blob: APACHE2 + empty bytes → ME_E_INVALID_ARG") {
    reset_fetcher();
    g_fetcher.bytes       = {};   /* host bug: OK return but no bytes */
    g_fetcher.license     = ME_MODEL_LICENSE_APACHE2;
    g_fetcher.expose_hash = false;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);
    me::inference::LoadedModel out;
    CHECK(me::inference::load_model_blob(g.eng, "m", "v1", "fp32", &out)
          == ME_E_INVALID_ARG);
    const char* err = me_engine_last_error(g.eng);
    CHECK(std::string{err}.find("bytes/size is empty") != std::string::npos);
}

TEST_CASE("load_model_blob: empty quantization is accepted (some models lack the variant)") {
    reset_fetcher();
    g_fetcher.bytes       = {1};
    g_fetcher.license     = ME_MODEL_LICENSE_APACHE2;
    g_fetcher.expose_hash = false;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);
    me::inference::LoadedModel out;
    /* Both NULL and "" should be accepted as quantization. */
    REQUIRE(me::inference::load_model_blob(g.eng, "m", "v1", nullptr, &out) == ME_OK);
    CHECK(g_fetcher.last_quantization.empty());
    REQUIRE(me::inference::load_model_blob(g.eng, "m", "v1", "", &out) == ME_OK);
    CHECK(g_fetcher.last_quantization.empty());
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("load_model_blob: skipped (ME_WITH_INFERENCE=OFF)") {
    /* Build-flag-gated stub. */
}

#endif
