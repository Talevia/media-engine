/*
 * test_inference_coreml_skeleton — exercises the
 * `me::inference::CoreMlRuntime` skeleton landed in the
 * `ml-inference-runtime-coreml` cycle. Validates that the
 * factory returns a non-null instance for valid input + null
 * for empty input, and that `run()` returns ME_E_UNSUPPORTED
 * with the follow-up bullet slug in the error message — the
 * STUB marker contract per `tools/check_stubs.sh`.
 *
 * The actual MLModel-loading + run() body is the
 * `ml-inference-runtime-coreml-impl` follow-up bullet; that
 * cycle expands this test into a real-model smoke (synthetic
 * input frame → landmark output verified against a deterministic
 * reference).
 *
 * Gated by `ME_HAS_INFERENCE` — when the engine is built with
 * `ME_WITH_INFERENCE=OFF` (the default), this test compiles
 * to a no-op (TEST_CASE bodies wrapped in #ifdef).
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#ifdef ME_HAS_INFERENCE
#include "inference/coreml_runtime.hpp"
#include "inference/runtime.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#endif

#ifdef ME_HAS_INFERENCE

TEST_CASE("CoreMlRuntime: create() rejects empty blob") {
    std::string err;
    auto rt = me::inference::CoreMlRuntime::create(nullptr, 0, &err);
    CHECK(rt == nullptr);
    CHECK(err.find("empty") != std::string::npos);
}

TEST_CASE("CoreMlRuntime: create() rejects null bytes pointer") {
    std::string err;
    auto rt = me::inference::CoreMlRuntime::create(nullptr, 16, &err);
    CHECK(rt == nullptr);
    CHECK(!err.empty());
}

TEST_CASE("CoreMlRuntime: create() returns non-null for valid bytes") {
    /* Skeleton state: the blob doesn't have to be a real MLModel —
     * the cycle 45 stub stores bytes verbatim and defers MLModel
     * compilation to the *-impl follow-up. Use a synthetic 32-byte
     * payload so the factory's "size > 0" branch is exercised. */
    const std::array<std::uint8_t, 32> blob{};  /* zero-filled */
    std::string err;
    auto rt = me::inference::CoreMlRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);
    CHECK(err.empty());
}

TEST_CASE("CoreMlRuntime: run() returns ME_E_UNSUPPORTED with follow-up slug") {
    const std::array<std::uint8_t, 16> blob{};
    std::string err;
    auto rt = me::inference::CoreMlRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);

    std::map<std::string, me::inference::Tensor> inputs;
    std::map<std::string, me::inference::Tensor> outputs;
    std::string run_err;

    const me_status_t rc = rt->run(inputs, &outputs, &run_err);
    CHECK(rc == ME_E_UNSUPPORTED);
    /* Error message must point readers at the follow-up bullet —
     * the canonical "where do I go from here" debugging hint
     * per the STUB marker convention. */
    CHECK(run_err.find("ml-inference-runtime-coreml-impl") != std::string::npos);
}

TEST_CASE("Tensor: dtype_byte_size covers each enum entry") {
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Float32) == 4);
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Int32)   == 4);
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Uint8)   == 1);
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Float16) == 2);
}

#else  /* !ME_HAS_INFERENCE */

TEST_CASE("CoreMlRuntime: skipped (ME_WITH_INFERENCE=OFF)") {
    /* Build-flag-gated stub. The TEST_CASE name surfaces in the
     * ctest log so reviewers can verify the suite was correctly
     * skipped (vs silently absent). */
}

#endif
