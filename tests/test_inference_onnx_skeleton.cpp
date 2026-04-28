/*
 * test_inference_onnx_skeleton — exercises the
 * `me::inference::OnnxRuntime` cross-platform CPU FP32 backend.
 * Symmetric with `test_inference_coreml_skeleton`: factory
 * arg-validation + the lazy session-init error path (junk blob
 * → ME_E_DECODE with cached error replayed on repeat calls).
 *
 * Real-model coverage (a known-input model loaded via the
 * model-fetcher API → produces expected output) is the
 * `ml-ship-path-model-blazeface` follow-up bullet. This file
 * pins the wiring shape that bullet will extend.
 *
 * Gated on `ME_HAS_INFERENCE && ME_HAS_ONNX_RUNTIME`
 * (both PRIVATE compile defs from the engine target). On hosts
 * that don't have onnxruntime installed, the suite reports a
 * single skipped-stub case.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"

#if defined(ME_HAS_INFERENCE) && defined(ME_HAS_ONNX_RUNTIME)
#include "inference/onnx_runtime.hpp"
#include "inference/runtime.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>

TEST_CASE("OnnxRuntime: create() rejects empty blob") {
    std::string err;
    auto rt = me::inference::OnnxRuntime::create(nullptr, 0, &err);
    CHECK(rt == nullptr);
    CHECK(err.find("empty") != std::string::npos);
}

TEST_CASE("OnnxRuntime: create() rejects null bytes pointer") {
    std::string err;
    auto rt = me::inference::OnnxRuntime::create(nullptr, 16, &err);
    CHECK(rt == nullptr);
    CHECK(!err.empty());
}

TEST_CASE("OnnxRuntime: create() returns non-null for valid bytes") {
    /* Lazy session-init: factory accepts any non-empty blob and
     * defers Ort::Session construction to the first run() call.
     * The blob need not be a real .onnx model here — that gets
     * surfaced as a run-time error in the next test case. */
    const std::array<std::uint8_t, 32> blob{};  /* zero-filled */
    std::string err;
    auto rt = me::inference::OnnxRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);
    CHECK(err.empty());
}

TEST_CASE("OnnxRuntime: run() with junk blob returns ME_E_DECODE") {
    /* Junk bytes can't parse as an ONNX protobuf model; ORT
     * throws Ort::Exception during Session construction, which
     * we surface as ME_E_DECODE + a descriptive error string. */
    const std::array<std::uint8_t, 16> blob{};
    std::string err;
    auto rt = me::inference::OnnxRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);

    std::map<std::string, me::inference::Tensor> inputs;
    std::map<std::string, me::inference::Tensor> outputs;
    std::string run_err;

    const me_status_t rc = rt->run(inputs, &outputs, &run_err);
    CHECK(rc == ME_E_DECODE);
    CHECK(run_err.find("OnnxRuntime") != std::string::npos);
    /* Sticky cache: second call replays the cached error. */
    std::string err2;
    const me_status_t rc2 = rt->run(inputs, &outputs, &err2);
    CHECK(rc2 == ME_E_DECODE);
    CHECK(err2 == run_err);
}

TEST_CASE("OnnxRuntime: run() rejects null outputs map") {
    const std::array<std::uint8_t, 16> blob{};
    std::string err;
    auto rt = me::inference::OnnxRuntime::create(
        blob.data(), blob.size(), &err);
    REQUIRE(rt != nullptr);

    std::map<std::string, me::inference::Tensor> inputs;
    std::string run_err;
    const me_status_t rc = rt->run(inputs, nullptr, &run_err);
    CHECK(rc == ME_E_INVALID_ARG);
    CHECK(!run_err.empty());
}

#else  /* !(ME_HAS_INFERENCE && ME_HAS_ONNX_RUNTIME) */

TEST_CASE("OnnxRuntime: skipped (ME_HAS_ONNX_RUNTIME not set)") {
    /* Build-flag-gated stub. */
}

#endif
