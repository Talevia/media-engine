/* CoreMlRuntime skeleton — pure C++ pre-cycle that lays down the
 * class shape without touching Apple's CoreML.framework yet. The
 * `ml-inference-runtime-coreml-impl` follow-up flips this file
 * to `.mm` (Objective-C++) and replaces the stub with real
 * MLModel construction via temp-file + compileModelAtURL.
 *
 * Today's behavior:
 *   - Constructor / `create()`: copies the input bytes into the
 *     Pimpl struct's `std::vector<uint8_t>`. No MLModel built.
 *   - `run()`: returns ME_E_UNSUPPORTED + populates error_msg
 *     with the follow-up bullet slug.
 *
 * The factory still returns a non-null instance for valid
 * input — downstream wiring (the M11 effect kernels +
 * test_inference_coreml_skeleton) can be exercised end-to-end
 * (registration / lifecycle) before the run() body lands. The
 * STUB marker below is the canonical link between this stub
 * and the follow-up bullet. */
#include "inference/coreml_runtime.hpp"

#include <cstring>
#include <utility>
#include <vector>

namespace me::inference {

struct CoreMlRuntime::Impl {
    /* Snapshot of the host-supplied model bytes. The actual MLModel
     * gets built from these in the *-impl cycle (write to temp
     * file → compileModelAtURL → modelWithContentsOfURL). Storing
     * here keeps the load deferred without losing the input. */
    std::vector<std::uint8_t> blob;
};

CoreMlRuntime::CoreMlRuntime() : impl_(std::make_unique<Impl>()) {}
CoreMlRuntime::~CoreMlRuntime() = default;

std::unique_ptr<CoreMlRuntime> CoreMlRuntime::create(
    const std::uint8_t* bytes,
    std::size_t         size,
    std::string*        error_msg) {
    if (!bytes || size == 0) {
        if (error_msg) {
            *error_msg = "CoreMlRuntime::create: empty model blob";
        }
        return nullptr;
    }
    auto rt = std::unique_ptr<CoreMlRuntime>(new CoreMlRuntime());
    rt->impl_->blob.assign(bytes, bytes + size);
    return rt;
}

me_status_t CoreMlRuntime::run(
    const std::map<std::string, Tensor>& /*inputs*/,
    std::map<std::string, Tensor>*       /*outputs*/,
    std::string*                         error_msg) {
    /* STUB: ml-inference-runtime-coreml-impl */
    if (error_msg) {
        *error_msg =
            "CoreMlRuntime::run not implemented — see backlog "
            "ml-inference-runtime-coreml-impl for the MLModel "
            "loading + run() body wiring";
    }
    return ME_E_UNSUPPORTED;
}

}  // namespace me::inference
