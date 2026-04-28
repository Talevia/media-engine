/* CoreMlRuntime — Apple CoreML.framework backend for the M11
 * `me::inference::Runtime` interface. Apple-only TU. CMake gates
 * compilation on `APPLE AND ME_WITH_INFERENCE`; non-Apple builds
 * with `ME_WITH_INFERENCE=ON` get the cross-platform ONNX
 * runtime path (sibling backlog `ml-inference-runtime-onnx`).
 *
 * Lifecycle.
 *   - `create()`: stashes the raw model bytes in the Pimpl.
 *     Compilation is deferred to first `run()` so transient
 *     errors from a junk blob don't leak into the C-API
 *     boundary at construction time — callers see them as a
 *     normal me_status_t return from inference.
 *   - First `run()`: writes the blob to an `NSTemporaryDirectory`
 *     `.mlmodel` file, calls `MLModel.compileModelAtURL:` to
 *     produce the platform-compiled `.mlmodelc` directory,
 *     then `MLModel.modelWithContentsOfURL:` to load it. The
 *     temp `.mlmodel` source file is removed after the compile
 *     attempt; the compiled `.mlmodelc` lives until process
 *     exit (Apple keeps the open MLModel referencing it; we
 *     don't track its path for delayed cleanup, accepting the
 *     small temp-dir footprint).
 *   - Subsequent `run()`s: reuse the cached MLModel + replay
 *     the cached compile_error if compilation already failed
 *     (no thrash retry).
 *
 * Tensor mapping. The interface's `Dtype` translates to
 * `MLMultiArrayDataType` for non-image MLMultiArray I/O:
 *   - Float32 → MLMultiArrayDataTypeFloat32
 *   - Int32   → MLMultiArrayDataTypeInt32
 *   - Float16 → MLMultiArrayDataTypeFloat16
 *   - Uint8 currently rejects with ME_E_INVALID_ARG — image-
 *     shape inputs need the MLImageConstraint / CVPixelBuffer
 *     path, out of scope this cycle (the M11 face-* effect
 *     kernels feed Float32 NCHW tensors normalized host-side).
 *
 * Strides. MLMultiArray supports strided storage. The impl
 * fast-paths row-major-contiguous outputs (a flat memcpy from
 * Apple's getBytesWithHandler buffer) and falls back to an
 * element-by-element walk via
 * `me::inference::strided_copy_to_contiguous` when strides
 * deviate. The strided path is correct but slower (one memcpy
 * per element); Apple's M11 ship-path models in practice return
 * contiguous output, so the fast path covers the common case.
 *
 * ARC. CMake compiles this TU with `-fobjc-arc`. The Pimpl
 * holds `id` references (`MLModel *model`) which ARC retains
 * for lifetime of `Impl` and releases on Impl destruction.
 *
 * Threading. `run()` is reentrant — the lazy-compile guard
 * uses an internal mutex so concurrent first-run callers
 * can't race the compile. MLModel itself is thread-safe per
 * Apple's docs ("instances are safe to use from multiple
 * threads"). */
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include "inference/coreml_runtime.hpp"
#include "inference/multiarray_layout.hpp"

#include <cstring>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace me::inference {

namespace {

/* Map our Dtype → MLMultiArrayDataType. Returns false if
 * unsupported (Uint8 today; future image-input path will
 * route those via CVPixelBuffer instead of MLMultiArray). */
bool dtype_to_mldatatype(Dtype d, MLMultiArrayDataType* out) {
    switch (d) {
    case Dtype::Float32: *out = MLMultiArrayDataTypeFloat32; return true;
    case Dtype::Int32:   *out = MLMultiArrayDataTypeInt32;   return true;
    case Dtype::Float16: *out = MLMultiArrayDataTypeFloat16; return true;
    case Dtype::Uint8:   return false;
    }
    return false;
}

bool mldatatype_to_dtype(MLMultiArrayDataType m, Dtype* out) {
    switch (m) {
    case MLMultiArrayDataTypeFloat32: *out = Dtype::Float32; return true;
    case MLMultiArrayDataTypeInt32:   *out = Dtype::Int32;   return true;
    case MLMultiArrayDataTypeFloat16: *out = Dtype::Float16; return true;
    default: return false;  /* Float64 / Double — not in our enum */
    }
}

}  // namespace

struct CoreMlRuntime::Impl {
    std::vector<std::uint8_t> blob;
    std::mutex                mu;
    bool                      compile_attempted = false;
    std::string               compile_error;
    MLModel*                  model = nil;  /* ARC-retained */
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
    const std::map<std::string, Tensor>& inputs,
    std::map<std::string, Tensor>*       outputs,
    std::string*                         error_msg) {

    if (!outputs) {
        if (error_msg) *error_msg = "CoreMlRuntime::run: null outputs map";
        return ME_E_INVALID_ARG;
    }

    /* Lazy compile — first call carries the cost. The compile path
     * runs once per runtime instance under impl_->mu so concurrent
     * first callers don't race. Subsequent callers see
     * compile_attempted == true and skip straight to dispatch. */
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->compile_attempted) {
            impl_->compile_attempted = true;
            @autoreleasepool {
                NSString* tmpDir   = NSTemporaryDirectory();
                NSString* uuid     = [[NSUUID UUID] UUIDString];
                NSString* filename = [NSString stringWithFormat:@"%@%@.mlmodel",
                                                                  tmpDir, uuid];
                NSData* data = [NSData dataWithBytes:impl_->blob.data()
                                              length:static_cast<NSUInteger>(impl_->blob.size())];
                NSError* writeErr = nil;
                const BOOL wrote = [data writeToFile:filename
                                             options:NSDataWritingAtomic
                                               error:&writeErr];
                if (!wrote) {
                    impl_->compile_error =
                        std::string{"CoreMlRuntime: failed to write model "
                                     "blob to temp file: "} +
                        [[writeErr localizedDescription] UTF8String];
                } else {
                    NSURL* srcUrl = [NSURL fileURLWithPath:filename];
                    NSError* compileErr = nil;
                    NSURL* compiledUrl =
                        [MLModel compileModelAtURL:srcUrl error:&compileErr];
                    /* Idempotent cleanup of the source .mlmodel. */
                    [[NSFileManager defaultManager] removeItemAtURL:srcUrl
                                                              error:nil];
                    if (!compiledUrl) {
                        impl_->compile_error =
                            std::string{"CoreMlRuntime: MLModel "
                                         "compileModelAtURL failed: "} +
                            [[compileErr localizedDescription] UTF8String];
                    } else {
                        NSError* loadErr = nil;
                        impl_->model =
                            [MLModel modelWithContentsOfURL:compiledUrl
                                                       error:&loadErr];
                        if (!impl_->model) {
                            impl_->compile_error =
                                std::string{"CoreMlRuntime: MLModel "
                                             "modelWithContentsOfURL failed: "} +
                                [[loadErr localizedDescription] UTF8String];
                        }
                    }
                }
            }
        }
        if (!impl_->model) {
            if (error_msg) *error_msg = impl_->compile_error;
            return ME_E_DECODE;
        }
    }

    @autoreleasepool {
        /* Build the input feature provider. */
        NSMutableDictionary<NSString*, MLFeatureValue*>* dict =
            [NSMutableDictionary dictionaryWithCapacity:inputs.size()];

        for (const auto& [name, tensor] : inputs) {
            MLMultiArrayDataType ml_dtype;
            if (!dtype_to_mldatatype(tensor.dtype, &ml_dtype)) {
                if (error_msg) {
                    *error_msg = "CoreMlRuntime::run: input '" + name +
                                  "' has unsupported dtype "
                                  "(Uint8 inputs need the MLImageConstraint "
                                  "path, not yet wired)";
                }
                return ME_E_INVALID_ARG;
            }
            const std::size_t elem_bytes = dtype_byte_size(tensor.dtype);
            std::int64_t elem_count = 1;
            for (std::int64_t d : tensor.shape) elem_count *= d;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(elem_count) * elem_bytes;
            if (tensor.bytes.size() != expected_bytes) {
                if (error_msg) {
                    *error_msg = "CoreMlRuntime::run: input '" + name +
                                  "' bytes.size() does not match "
                                  "product(shape) * dtype_byte_size";
                }
                return ME_E_INVALID_ARG;
            }

            NSMutableArray<NSNumber*>* nsShape =
                [NSMutableArray arrayWithCapacity:tensor.shape.size()];
            for (std::int64_t d : tensor.shape) {
                [nsShape addObject:[NSNumber numberWithLongLong:d]];
            }

            NSError* arrErr = nil;
            MLMultiArray* arr =
                [[MLMultiArray alloc] initWithShape:nsShape
                                            dataType:ml_dtype
                                               error:&arrErr];
            if (!arr) {
                if (error_msg) {
                    *error_msg = std::string{"CoreMlRuntime::run: failed to "
                                              "allocate MLMultiArray: "} +
                                  [[arrErr localizedDescription] UTF8String];
                }
                return ME_E_INVALID_ARG;
            }

            /* Block captures: structured-binding `tensor` can't be
             * captured directly by ObjC++ blocks on this toolchain;
             * stash a raw pointer + size so the block sees plain
             * scalars. expected_bytes is value-captured. */
            const std::uint8_t* src_data         = tensor.bytes.data();
            const std::size_t   src_bytes        = expected_bytes;
            __block bool        copy_ok          = false;
            [arr getMutableBytesWithHandler:^(void* mutableBytes,
                                                NSInteger byteSize,
                                                NSArray<NSNumber*>* /*strides*/) {
                if (static_cast<std::size_t>(byteSize) >= src_bytes) {
                    std::memcpy(mutableBytes, src_data, src_bytes);
                    copy_ok = true;
                }
            }];
            if (!copy_ok) {
                if (error_msg) {
                    *error_msg = "CoreMlRuntime::run: MLMultiArray byteSize "
                                  "shorter than expected payload for input '" +
                                  name + "'";
                }
                return ME_E_INVALID_ARG;
            }

            MLFeatureValue* fv =
                [MLFeatureValue featureValueWithMultiArray:arr];
            NSString* nsName =
                [NSString stringWithUTF8String:name.c_str()];
            dict[nsName] = fv;
        }

        NSError* providerErr = nil;
        MLDictionaryFeatureProvider* provider =
            [[MLDictionaryFeatureProvider alloc] initWithDictionary:dict
                                                              error:&providerErr];
        if (!provider) {
            if (error_msg) {
                *error_msg = std::string{"CoreMlRuntime::run: failed to build "
                                          "feature provider: "} +
                              [[providerErr localizedDescription] UTF8String];
            }
            return ME_E_INVALID_ARG;
        }

        NSError* predErr = nil;
        id<MLFeatureProvider> result =
            [impl_->model predictionFromFeatures:provider error:&predErr];
        if (!result) {
            if (error_msg) {
                *error_msg = std::string{"CoreMlRuntime::run: prediction "
                                          "failed: "} +
                              [[predErr localizedDescription] UTF8String];
            }
            return ME_E_DECODE;
        }

        /* Translate output features. Only MLMultiArray-typed features
         * are surfaced; image / sequence / dictionary outputs are
         * out of scope this cycle (effect kernels consuming inference
         * results expect MLMultiArray today). */
        outputs->clear();
        for (NSString* nsName in [result featureNames]) {
            MLFeatureValue* fv = [result featureValueForName:nsName];
            if (fv.type != MLFeatureTypeMultiArray) continue;
            MLMultiArray* arr = fv.multiArrayValue;

            Dtype dtype;
            if (!mldatatype_to_dtype(arr.dataType, &dtype)) continue;

            Tensor t;
            t.dtype = dtype;
            t.shape.reserve([arr.shape count]);
            for (NSNumber* d in arr.shape) {
                t.shape.push_back([d longLongValue]);
            }
            const std::size_t elem_bytes = dtype_byte_size(dtype);
            std::int64_t elem_count = 1;
            for (std::int64_t d : t.shape) elem_count *= d;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(elem_count) * elem_bytes;
            t.bytes.resize(expected_bytes);

            /* Resolve the source layout: arr.strides is element-strides
             * (counts), in the order matching arr.shape. Pull both
             * into plain C++ vectors so the layout helpers can be
             * called without ObjC. */
            std::vector<std::int64_t> elem_strides;
            elem_strides.reserve([arr.strides count]);
            for (NSNumber* s in arr.strides) {
                elem_strides.push_back([s longLongValue]);
            }
            const std::span<const std::int64_t> shape_span(t.shape);
            const std::span<const std::int64_t> strides_span(elem_strides);
            const bool contig = is_row_major_contiguous(shape_span, strides_span);

            std::uint8_t*     dst_data  = t.bytes.data();
            const std::size_t dst_bytes = expected_bytes;
            __block bool      ok        = false;
            __block std::string copy_err;
            [arr getBytesWithHandler:^(const void* bytes, NSInteger byteSize) {
                /* getBytesWithHandler block signature is
                 * `void(^)(const void*, NSInteger)` — no strides
                 * argument (asymmetric with the mutable variant). */
                if (contig) {
                    if (static_cast<std::size_t>(byteSize) >= dst_bytes) {
                        std::memcpy(dst_data, bytes, dst_bytes);
                        ok = true;
                    } else {
                        copy_err = "MLMultiArray byteSize shorter than "
                                    "expected payload";
                    }
                } else {
                    /* Strided walk — element-by-element copy via
                     * multiarray_layout::strided_copy_to_contiguous.
                     * Slower than memcpy by a factor of element-count
                     * but correct under any element-stride pattern
                     * Apple may produce. */
                    const auto* src_u8 = static_cast<const std::uint8_t*>(bytes);
                    ok = strided_copy_to_contiguous(
                        src_u8,
                        static_cast<std::size_t>(byteSize),
                        shape_span,
                        strides_span,
                        elem_bytes,
                        dst_data);
                    if (!ok) {
                        copy_err = "strided_copy_to_contiguous rejected the "
                                    "layout (length mismatch / out-of-bounds "
                                    "byteSize)";
                    }
                }
            }];
            if (!ok) {
                if (error_msg) {
                    *error_msg = std::string{"CoreMlRuntime::run: output '"} +
                                  [nsName UTF8String] + "' " + copy_err;
                }
                return ME_E_DECODE;
            }

            (*outputs)[std::string{[nsName UTF8String]}] = std::move(t);
        }
    }
    return ME_OK;
}

}  // namespace me::inference
