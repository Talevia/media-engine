/* `me::inference::Runtime` — abstract base for ML inference runtimes
 * landing under M11 (`docs/MILESTONES.md` §M11).
 *
 * Two concrete impls planned per docs/MILESTONES.md:135 (M11 exit
 * criterion 4): CoreMlRuntime (Apple) + OnnxRuntime (cross-
 * platform CPU FP32 reference). Both inherit from this abstract
 * base so the effect-kernel layer (face_sticker / face_mosaic /
 * body_alpha_key in `src/compose/`) can call `run()` without
 * caring which runtime backs it. The Engine picks one at startup
 * based on capabilities + license whitelist.
 *
 * Runtime is gated on `ME_HAS_INFERENCE` (set when CMake's
 * `ME_WITH_INFERENCE=ON`). Off-builds don't see this header at
 * all because nothing under `src/compose/` references it; the
 * inference path is engaged only when an effect kernel needs
 * landmark / mask data and the engine has a runtime registered.
 *
 * Determinism contract. ML inference is **explicitly non-
 * deterministic** under VISION §3.4's carve-out. Output bytes
 * vary across CoreML / ONNX-CPU paths, across model versions,
 * and within a single runtime due to internal floating-point
 * reordering / SIMD kernel selection. Effect kernels that
 * consume runtime output therefore produce non-byte-identical
 * frames; the deterministic-software-path contract applies to
 * the rest of the engine, not the inference layer.
 *
 * Skeleton-only as of cycle 45 (this header + the CoreML
 * skeleton land). Actual model loading + run() body is the
 * `ml-inference-runtime-coreml-impl` follow-up bullet — that
 * cycle wires Apple's CoreML.framework (Objective-C++) and the
 * temp-file model-compile path. The runtime-interface shape
 * (Tensor / run() signature) is locked here so downstream M11
 * cycles can build against it.
 *
 * Threading. A `Runtime` instance is shared by reference among
 * effect kernels; impls must be thread-safe in `run()`. Both
 * planned backends (CoreML, ONNX-CPU) handle thread-safety
 * internally; if a future runtime can't, it must wrap its
 * non-thread-safe core in a mutex inside `run()`. */
#pragma once

#include "media_engine/types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace me::inference {

/* Per-element data type for `Tensor::bytes`. Chosen to cover the
 * common ML model signatures: Float32 for full-precision CPU
 * reference, Int32 for index / class outputs, Uint8 for image-
 * shaped inputs (RGBA frames, alpha masks), Float16 for
 * Apple-Neural-Engine quantized weights. Adding new dtypes is
 * append-only ABI evolution per the variant-index pattern used
 * elsewhere in the engine. */
enum class Dtype : std::uint8_t {
    Float32 = 0,
    Int32   = 1,
    Uint8   = 2,
    Float16 = 3,
};

constexpr std::size_t dtype_byte_size(Dtype d) noexcept {
    switch (d) {
    case Dtype::Float32: return 4;
    case Dtype::Int32:   return 4;
    case Dtype::Uint8:   return 1;
    case Dtype::Float16: return 2;
    }
    return 0;
}

/* Tensor — a typed n-dimensional buffer.
 *
 * `shape` lists dimension sizes (e.g. `{1, 3, 224, 224}` for an
 * NCHW image). `bytes.size()` MUST equal `product(shape) *
 * dtype_byte_size(dtype)` — runtimes may assert this at the
 * boundary. Shape is std::vector<int64_t> not int32 because
 * some models (e.g. SAM) carry dimensions ≥ 2³¹. */
struct Tensor {
    std::vector<std::int64_t> shape;
    Dtype                     dtype = Dtype::Float32;
    std::vector<std::uint8_t> bytes;
};

/* Abstract base. The Engine owns a single Runtime instance per
 * loaded model (keyed by `model_id`); effect kernels borrow it
 * by reference at task-dispatch time. */
class Runtime {
public:
    virtual ~Runtime() = default;

    /* Run inference: read named input tensors, write named output
     * tensors. The model's input/output names come from the
     * `me_model_blob_t` metadata that loaded the runtime —
     * effect kernels know the contract a priori (face landmark
     * model has known input "image" + output "landmarks" etc.).
     *
     * Input/output keying via `std::map<std::string, Tensor>`
     * (sorted, NOT std::unordered_map) so iteration order is
     * stable across runs — important for log-deterministic
     * output even though the model's run() itself is non-
     * deterministic.
     *
     * Returns ME_OK on success. ME_E_INVALID_ARG when input
     * tensor shapes / dtypes don't match the model's declared
     * inputs. ME_E_UNSUPPORTED when the runtime impl isn't
     * landed yet (skeleton state). On error, populates
     * `*error_msg` with a host-displayable string. */
    virtual me_status_t run(
        const std::map<std::string, Tensor>& inputs,
        std::map<std::string, Tensor>*       outputs,
        std::string*                         error_msg) = 0;
};

}  // namespace me::inference
