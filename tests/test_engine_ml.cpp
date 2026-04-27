/*
 * test_engine_ml — M11 ml-model-lazy-load-callback registration.
 *
 * Pre-cycle-29: only registration is exercised; the fetcher is never
 * invoked because no inference runtime is wired yet (subsequent M11
 * cycles add the runtime that consults the stored callback). What
 * this suite asserts:
 *
 *   - me_engine_set_model_fetcher returns ME_OK when given a valid
 *     engine + cb + user.
 *   - NULL engine → ME_E_INVALID_ARG (uniform with the rest of the
 *     C API).
 *   - cb=NULL clears (returns ME_OK; semantics: registration is
 *     idempotent + callable mid-engine-life).
 *   - Re-registering overwrites (no leak, no error).
 *   - The whole test TU is gated by ME_HAS_INFERENCE so it
 *     compiles down to an empty doctest binary in OFF builds.
 *     Same shape as test_subtitle_renderer's ME_HAS_LIBASS gate.
 */
#include <doctest/doctest.h>

#ifdef ME_HAS_INFERENCE

#include <media_engine.h>
#include <media_engine/ml.h>

#include <cstdint>

namespace {

/* Test fetcher — never actually invoked pre-cycle-29 (no runtime
 * to call it). Captures invocation data into a static struct so
 * future cycles can verify the runtime calls the registered
 * callback. */
struct CallbackTrace {
    int call_count = 0;
};

me_status_t test_fetcher(const char* /*model_id*/,
                          const char* /*model_version*/,
                          const char* /*quantization*/,
                          me_model_blob_t* out_blob,
                          void* user) {
    auto* trace = static_cast<CallbackTrace*>(user);
    if (trace) ++trace->call_count;
    if (out_blob) *out_blob = me_model_blob_t{};
    return ME_OK;
}

}  // namespace

TEST_CASE("me_engine_set_model_fetcher: NULL engine → ME_E_INVALID_ARG") {
    CHECK(me_engine_set_model_fetcher(nullptr, &test_fetcher, nullptr)
          == ME_E_INVALID_ARG);
}

TEST_CASE("me_engine_set_model_fetcher: register + clear succeed") {
    me_engine_t* eng = nullptr;
    REQUIRE(me_engine_create(nullptr, &eng) == ME_OK);
    REQUIRE(eng != nullptr);

    CallbackTrace trace;
    /* Register. */
    CHECK(me_engine_set_model_fetcher(eng, &test_fetcher, &trace) == ME_OK);
    /* Clear (cb=NULL, user=NULL). */
    CHECK(me_engine_set_model_fetcher(eng, nullptr, nullptr) == ME_OK);
    /* Re-register. */
    CHECK(me_engine_set_model_fetcher(eng, &test_fetcher, &trace) == ME_OK);

    /* Pre-runtime, the callback is never invoked — no inference
     * effects are wired yet. trace.call_count stays 0. */
    CHECK(trace.call_count == 0);

    me_engine_destroy(eng);
}

TEST_CASE("me_model_license_t: whitelist enum values are stable") {
    /* Pin the enum positions so binding generators (JNI, K/N) can
     * map by integer value. Append-only ABI per the header doc. */
    CHECK(static_cast<int>(ME_MODEL_LICENSE_UNKNOWN) == 0);
    CHECK(static_cast<int>(ME_MODEL_LICENSE_APACHE2) == 1);
    CHECK(static_cast<int>(ME_MODEL_LICENSE_MIT)     == 2);
    CHECK(static_cast<int>(ME_MODEL_LICENSE_BSD)     == 3);
    CHECK(static_cast<int>(ME_MODEL_LICENSE_CC_BY)   == 4);
}

#endif /* ME_HAS_INFERENCE */
