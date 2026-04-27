/* `me::inference::CoreMlRuntime` — Apple CoreML.framework inference
 * backend, planned for M11 (`docs/MILESTONES.md` §M11 exit
 * criterion 4). Skeleton stage as of cycle 45: declares the class
 * shape + factory but `run()` returns ME_E_UNSUPPORTED with the
 * `ml-inference-runtime-coreml-impl` STUB marker. Actual MLModel
 * loading + Objective-C++ wiring lands in the follow-up cycle.
 *
 * Why a factory (`create()`) instead of a public constructor:
 * loading a model can fail (bad bytes, license violation, license
 * not whitelisted), and we want the caller to get a status code
 * instead of a thrown exception across the public C ABI. The
 * factory returns nullptr on failure + populates `*error_msg`.
 *
 * Pimpl idiom for the implementation detail: the impl is held
 * via `std::unique_ptr<Impl>` so this header stays free of
 * Apple-framework includes. Hosts (and tests) compile against
 * the header without dragging in `<CoreML/CoreML.h>` —
 * Objective-C++ stays inside `coreml_runtime.{cpp,mm}`. The
 * skeleton ships a `.cpp` today; the M11 follow-up flips it
 * to `.mm` when the actual MLModel wiring lands.
 *
 * Lifetime. The runtime owns its loaded model + any Apple-
 * framework Objective-C objects (released via ARC inside the
 * `.mm`). Constructor is private; only `create()` may
 * instantiate. Destructor is defined out-of-line so the Impl
 * struct's full type only needs to be visible inside the
 * coreml_runtime translation unit — preserves the
 * "no-Apple-framework in this header" invariant. */
#pragma once

#include "inference/runtime.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace me::inference {

class CoreMlRuntime final : public Runtime {
public:
    /* Factory: load the model bytes into an Apple MLModel.
     *
     * `bytes` MUST live for the duration of this call (no
     * post-call ownership semantics — the runtime copies what
     * it needs internally before returning). `size` is the
     * byte length. Returns nullptr on any failure (empty blob,
     * MLModel construction fails, license rejected) +
     * populates `*error_msg`.
     *
     * Skeleton state. Today this just stashes the bytes; the
     * actual MLModel construction is the
     * `ml-inference-runtime-coreml-impl` follow-up. The
     * factory still returns a non-null instance for valid
     * input so downstream wiring (test_inference_coreml_skeleton,
     * future engine integration) can be exercised end-to-end
     * even before run() works. */
    static std::unique_ptr<CoreMlRuntime> create(
        const std::uint8_t* bytes,
        std::size_t         size,
        std::string*        error_msg);

    ~CoreMlRuntime() override;

    /* Skeleton state: returns ME_E_UNSUPPORTED + writes the
     * follow-up bullet slug into `*error_msg` so debugging
     * sees exactly what's missing. */
    me_status_t run(
        const std::map<std::string, Tensor>& inputs,
        std::map<std::string, Tensor>*       outputs,
        std::string*                         error_msg) override;

private:
    CoreMlRuntime();

    /* Pimpl: full type defined in the .cpp/.mm so the
     * Apple-framework include stays out of this header. */
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me::inference
