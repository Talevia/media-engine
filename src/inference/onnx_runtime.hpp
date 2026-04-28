/* `me::inference::OnnxRuntime` — ONNX Runtime (CPU FP32) inference
 * backend. Cross-platform reference path that complements the
 * Apple-only CoreMlRuntime (`coreml_runtime.{hpp,mm}`); per
 * docs/MILESTONES.md §M11 exit criterion 4 ONNX-CPU is the
 * deterministic-reference path the engine picks on non-Apple
 * platforms (and on Apple when explicit cross-platform parity is
 * required, e.g. parity tests).
 *
 * Build gating. CMake links this TU only when both
 * `ME_WITH_INFERENCE=ON` and `pkg_check_modules(libonnxruntime)`
 * succeeds — the consumer doesn't have to manage two switches.
 * `ME_HAS_ONNX_RUNTIME` is set as a PRIVATE compile def for the
 * engine; tests that exercise this backend gate on the same macro
 * via `target_compile_definitions` propagation through the test's
 * own pkg-config detection.
 *
 * Why a factory (`create()`): loading a model can fail (bad bytes,
 * unsupported opset, license rejected). The factory returns
 * nullptr on failure + populates `*error_msg` so the caller gets
 * a status code, not a thrown exception across the public C ABI.
 *
 * Pimpl. The `Ort::Env` / `Ort::Session` lifetimes are held inside
 * the `Impl` so this header stays free of `<onnxruntime_cxx_api.h>`
 * (which transitively pulls in tens of thousands of LOC).
 *
 * Lifetime. The runtime owns its loaded session; effect kernels
 * borrow it by reference at task-dispatch time. Destructor is
 * defined out-of-line so the Impl struct's full type only needs
 * to be visible inside the `.cpp` translation unit.
 *
 * Threading. `run()` is reentrant — concurrent calls share the
 * underlying `Ort::Session`, which the ONNX Runtime documents as
 * thread-safe for inference. The lazy session-init guard uses an
 * internal mutex so concurrent first-callers can't race the
 * load.
 *
 * Determinism. The runtime is configured with
 * `IntraOpNumThreads=1` + `InterOpNumThreads=1` + ORT_ENABLE_BASIC
 * graph optimization so a single CPU executes ops in a fixed
 * order — the cheapest reproducibility win available without
 * disabling the optimizer entirely. Per VISION §3.4 ML inference
 * is in the non-deterministic carve-out anyway (FP non-
 * associativity across CPU microarchitectures); these settings
 * minimize avoidable variation.
 */
#pragma once

#include "inference/runtime.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace me::inference {

class OnnxRuntime final : public Runtime {
public:
    /* Factory: load the model bytes into an Ort::Session.
     *
     * Unlike CoreML, ONNX Runtime constructs its session directly
     * from a byte buffer — no temp-file dance is needed. We still
     * defer the session construction to the first `run()` call so
     * a junk blob's load failure surfaces as a normal me_status_t
     * return rather than a constructor-time throw.
     *
     * `bytes` lifetime: the runtime copies the bytes into the
     * Pimpl on `create()`; callers are free to release the buffer
     * after this returns. Returns nullptr on (empty blob, null
     * pointer) + populates `*error_msg`. */
    static std::unique_ptr<OnnxRuntime> create(
        const std::uint8_t* bytes,
        std::size_t         size,
        std::string*        error_msg);

    ~OnnxRuntime() override;

    /* Lazy session-init on first call. On junk blob the error
     * is cached + every subsequent `run()` replays it (no
     * thrash retry). On success runs Ort::Session::Run with the
     * configured single-thread CPU EP and translates input /
     * output tensors via the names ONNX Runtime publishes from
     * the model graph. */
    me_status_t run(
        const std::map<std::string, Tensor>& inputs,
        std::map<std::string, Tensor>*       outputs,
        std::string*                         error_msg) override;

private:
    OnnxRuntime();

    /* Pimpl: full type defined in the .cpp so the
     * `<onnxruntime_cxx_api.h>` include stays out of this
     * header. */
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me::inference
