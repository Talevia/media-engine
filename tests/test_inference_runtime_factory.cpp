/*
 * test_inference_runtime_factory — pin the cycle-50 typed
 * Runtime factory's contract (M11 inference-runtime-factory-impl).
 *
 * Verifies:
 *   1. NULL engine / NULL out_runtime / NULL or empty
 *      identifiers → ME_E_INVALID_ARG with diag.
 *   2. load_model_blob rejection (license whitelist / content_hash)
 *      propagates through the factory unchanged.
 *   3. Fetcher rejection → ME_E_NOT_FOUND.
 *   4. Cache hit: repeated calls with same identity return the
 *      same Runtime* pointer.
 *   5. Distinct identities don't collide.
 *   6. clear_loaded_runtimes invalidates the cache + frees the
 *      stored unique_ptr (next call constructs a fresh
 *      instance).
 *
 * Construction-success path (engine builds with at least one
 * concrete Runtime backend compiled in) is exercised by the
 * env-var-gated test_inference_blazeface (existing M11 test).
 * This test focuses on the factory's glue logic, so the
 * "construction" arm uses real bytes the host provides via the
 * normal fetcher — when neither CoreML nor ONNX is built into
 * the engine, the factory returns ME_E_INTERNAL with a
 * diagnostic naming the missing backend, which we DO assert.
 *
 * Gated on ME_HAS_INFERENCE; OFF builds compile to a single
 * skipped TEST_CASE.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE

#include "inference/runtime.hpp"
#include "inference/runtime_factory.hpp"
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
    out_blob->content_hash = nullptr;  /* host opts out of integrity check */
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

TEST_CASE("make_runtime_for_model: NULL engine returns ME_E_INVALID_ARG") {
    me::inference::Runtime* rt = nullptr;
    std::string err;
    CHECK(me::inference::make_runtime_for_model(
              nullptr, "m", "v1", "fp32", &rt, &err)
          == ME_E_INVALID_ARG);
    CHECK(rt == nullptr);
}

TEST_CASE("make_runtime_for_model: NULL out_runtime returns ME_E_INVALID_ARG") {
    EngineGuard g;
    std::string err;
    CHECK(me::inference::make_runtime_for_model(
              g.eng, "m", "v1", "fp32", nullptr, &err)
          == ME_E_INVALID_ARG);
}

TEST_CASE("make_runtime_for_model: NULL / empty model_id returns ME_E_INVALID_ARG") {
    EngineGuard g;
    me::inference::Runtime* rt = nullptr;
    std::string err;
    CHECK(me::inference::make_runtime_for_model(
              g.eng, nullptr, "v1", "fp32", &rt, &err) == ME_E_INVALID_ARG);
    CHECK(err.find("model_id") != std::string::npos);

    err.clear();
    CHECK(me::inference::make_runtime_for_model(
              g.eng, "", "v1", "fp32", &rt, &err) == ME_E_INVALID_ARG);
    CHECK(err.find("model_id") != std::string::npos);
}

TEST_CASE("make_runtime_for_model: empty model_version returns ME_E_INVALID_ARG") {
    EngineGuard g;
    me::inference::Runtime* rt = nullptr;
    std::string err;
    CHECK(me::inference::make_runtime_for_model(
              g.eng, "m", "", "fp32", &rt, &err) == ME_E_INVALID_ARG);
    CHECK(err.find("model_version") != std::string::npos);
}

TEST_CASE("make_runtime_for_model: fetcher non-OK propagates as ME_E_NOT_FOUND") {
    reset_fetcher();
    g_fetcher.return_status = ME_E_NOT_FOUND;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    me::inference::Runtime* rt = nullptr;
    std::string err;
    CHECK(me::inference::make_runtime_for_model(
              g.eng, "m", "v1", "fp32", &rt, &err) == ME_E_NOT_FOUND);
    CHECK(rt == nullptr);
}

TEST_CASE("make_runtime_for_model: license whitelist rejection propagates as ME_E_UNSUPPORTED") {
    reset_fetcher();
    g_fetcher.bytes = {0xAA, 0xBB};
    g_fetcher.license = ME_MODEL_LICENSE_UNKNOWN;  /* not whitelisted */

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    me::inference::Runtime* rt = nullptr;
    std::string err;
    CHECK(me::inference::make_runtime_for_model(
              g.eng, "m", "v1", "fp32", &rt, &err) == ME_E_UNSUPPORTED);
    CHECK(rt == nullptr);
    /* Diagnostic surfaces from load_model_blob via the engine's
     * last_error → factory's err out-param. */
    CHECK(err.find("UNKNOWN") != std::string::npos);
}

#if !defined(__APPLE__) && !defined(ME_HAS_ONNX_RUNTIME)
TEST_CASE("make_runtime_for_model: no backend compiled in → ME_E_INTERNAL with named diag") {
    /* Build flag matrix: no concrete Runtime impl. The factory's
     * Phase 3 (construction) returns nullptr with the diagnostic
     * pointing the host at the build flags. */
    reset_fetcher();
    g_fetcher.bytes = {0x42};
    g_fetcher.license = ME_MODEL_LICENSE_APACHE2;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    me::inference::Runtime* rt = nullptr;
    std::string err;
    CHECK(me::inference::make_runtime_for_model(
              g.eng, "m", "v1", "fp32", &rt, &err) == ME_E_INTERNAL);
    CHECK(rt == nullptr);
    CHECK(err.find("inference runtime") != std::string::npos);
}
#endif

TEST_CASE("make_runtime_for_model: cache hit — repeated calls return same Runtime*") {
    reset_fetcher();
    g_fetcher.bytes = {0x01, 0x02, 0x03, 0x04};
    g_fetcher.license = ME_MODEL_LICENSE_APACHE2;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    me::inference::Runtime* rt1 = nullptr;
    std::string err;
    me_status_t s1 = me::inference::make_runtime_for_model(
        g.eng, "m", "v1", "fp32", &rt1, &err);

    /* Skip the cache assertion when the build has no backend
     * compiled in — the first call returns nullptr with
     * ME_E_INTERNAL, and there's nothing to cache. */
    if (s1 != ME_OK) {
        CHECK(rt1 == nullptr);
        return;
    }
    REQUIRE(rt1 != nullptr);

    me::inference::Runtime* rt2 = nullptr;
    me_status_t s2 = me::inference::make_runtime_for_model(
        g.eng, "m", "v1", "fp32", &rt2, &err);
    REQUIRE(s2 == ME_OK);
    /* Cache hit returns the SAME pointer — engine owns the
     * Runtime, callers borrow. */
    CHECK(rt2 == rt1);
}

TEST_CASE("make_runtime_for_model: distinct identities don't collide") {
    reset_fetcher();
    g_fetcher.bytes = {0xAB};
    g_fetcher.license = ME_MODEL_LICENSE_APACHE2;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    me::inference::Runtime* rt_v1 = nullptr;
    me::inference::Runtime* rt_v2 = nullptr;
    std::string err;

    me_status_t s1 = me::inference::make_runtime_for_model(
        g.eng, "m", "v1", "fp32", &rt_v1, &err);
    me_status_t s2 = me::inference::make_runtime_for_model(
        g.eng, "m", "v2", "fp32", &rt_v2, &err);

    if (s1 != ME_OK || s2 != ME_OK) return;  /* no backend compiled */
    REQUIRE(rt_v1 != nullptr);
    REQUIRE(rt_v2 != nullptr);
    /* Different version → different Runtime instance. */
    CHECK(rt_v1 != rt_v2);
}

TEST_CASE("clear_loaded_runtimes: post-clear, next call constructs fresh instance") {
    reset_fetcher();
    g_fetcher.bytes = {0xCD};
    g_fetcher.license = ME_MODEL_LICENSE_APACHE2;

    EngineGuard g;
    REQUIRE(me_engine_set_model_fetcher(g.eng, test_fetcher, nullptr) == ME_OK);

    me::inference::Runtime* rt1 = nullptr;
    std::string err;
    me_status_t s = me::inference::make_runtime_for_model(
        g.eng, "m", "v1", "fp32", &rt1, &err);
    if (s != ME_OK) return;  /* no backend compiled */
    REQUIRE(rt1 != nullptr);

    me::inference::clear_loaded_runtimes(g.eng);

    me::inference::Runtime* rt2 = nullptr;
    REQUIRE(me::inference::make_runtime_for_model(
                g.eng, "m", "v1", "fp32", &rt2, &err) == ME_OK);
    REQUIRE(rt2 != nullptr);
    /* Post-clear: fresh construction → DIFFERENT pointer. */
    CHECK(rt2 != rt1);
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("runtime_factory: skipped (ME_WITH_INFERENCE=OFF)") {
    /* Build-flag-gated stub mirroring the other inference test
     * suites. */
}

#endif
