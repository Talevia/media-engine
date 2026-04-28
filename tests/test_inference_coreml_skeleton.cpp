/*
 * test_inference_coreml_skeleton — exercises the
 * `me::inference::CoreMlRuntime` Apple backend. The factory
 * lifecycle (null/empty rejection, non-null on valid bytes)
 * and the lazy-compile error path (junk blob → ME_E_DECODE
 * with model-load failure surfaced) are covered here.
 *
 * The actual happy-path (real .mlmodel → run() produces correct
 * output) is gated on a sourced-elsewhere Apache-licensed test
 * model — that's the `ml-ship-path-model-blazeface` follow-up
 * bullet. This file pins the wiring shape that bullet will
 * extend.
 *
 * Gated by both `ME_HAS_INFERENCE` AND `__APPLE__` — the .mm
 * implementing CoreMlRuntime is only compiled into the engine
 * library on Apple platforms (CMake gates `coreml_runtime.mm` on
 * `ME_WITH_INFERENCE AND APPLE`). Non-Apple builds with
 * ME_WITH_INFERENCE=ON will eventually use the ONNX runtime
 * backend (sibling backlog `ml-inference-runtime-onnx`); this
 * suite stays skipped there.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#if defined(ME_HAS_INFERENCE) && defined(__APPLE__)
#include "inference/coreml_runtime.hpp"
#include "inference/runtime.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>

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
    /* Lazy-compile path: factory accepts any non-empty blob and
     * defers MLModel compilation to the first run() call. The
     * blob need not be a real .mlmodel here — that gets surfaced
     * as a run-time error in the next test case. */
    const std::array<std::uint8_t, 32> blob{};  /* zero-filled */
    std::string err;
    auto rt = me::inference::CoreMlRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);
    CHECK(err.empty());
}

TEST_CASE("CoreMlRuntime: run() with junk blob returns ME_E_DECODE") {
    /* Lazy-compile model load fails on a non-.mlmodel blob.
     * MLModel.compileModelAtURL: returns nil + an NSError that
     * we surface into the C-API error string; the run() return
     * is ME_E_DECODE (load failed) rather than the previous
     * ME_E_UNSUPPORTED stub. */
    const std::array<std::uint8_t, 16> blob{};
    std::string err;
    auto rt = me::inference::CoreMlRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);

    std::map<std::string, me::inference::Tensor> inputs;
    std::map<std::string, me::inference::Tensor> outputs;
    std::string run_err;

    const me_status_t rc = rt->run(inputs, &outputs, &run_err);
    CHECK(rc == ME_E_DECODE);
    CHECK(run_err.find("CoreMlRuntime") != std::string::npos);
    /* Compile attempt is sticky — second call must replay the
     * cached error without trying to recompile. */
    std::string err2;
    const me_status_t rc2 = rt->run(inputs, &outputs, &err2);
    CHECK(rc2 == ME_E_DECODE);
    CHECK(err2 == run_err);
}

TEST_CASE("CoreMlRuntime: run() rejects null outputs map") {
    const std::array<std::uint8_t, 16> blob{};
    std::string err;
    auto rt = me::inference::CoreMlRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);

    std::map<std::string, me::inference::Tensor> inputs;
    std::string run_err;
    /* Null outputs → ME_E_INVALID_ARG is the documented contract;
     * shouldn't depend on whether the model compiled. */
    const me_status_t rc = rt->run(inputs, nullptr, &run_err);
    CHECK(rc == ME_E_INVALID_ARG);
    CHECK(!run_err.empty());
}

TEST_CASE("Tensor: dtype_byte_size covers each enum entry") {
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Float32) == 4);
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Int32)   == 4);
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Uint8)   == 1);
    CHECK(me::inference::dtype_byte_size(me::inference::Dtype::Float16) == 2);
}

#else  /* !(ME_HAS_INFERENCE && __APPLE__) */

TEST_CASE("CoreMlRuntime: skipped (ME_WITH_INFERENCE=OFF or non-Apple)") {
    /* Build-flag-gated stub. The TEST_CASE name surfaces in the
     * ctest log so reviewers can verify the suite was correctly
     * skipped (vs silently absent). */
}

#endif
