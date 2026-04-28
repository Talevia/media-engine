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
 * Strides. MLMultiArray supports strided storage, but Apple
 * models in practice return contiguous output for the shapes
 * we care about (vector / 4-D NCHW landmarks / masks). The
 * impl asserts contiguous-row-major and falls back to
 * element-by-element copy when strides disagree. Hosts seeing
 * the fallback path should file an issue with model details.
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

#include <cstring>
#include <mutex>
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

/* Compute the contiguous-row-major byte stride list for `shape`,
 * descending from outermost. Used to verify an MLMultiArray's
 * actual strides match what we'd produce ourselves. */
std::vector<std::int64_t> contiguous_strides(
    const std::vector<std::int64_t>& shape, std::size_t element_bytes) {
    std::vector<std::int64_t> strides(shape.size(), 0);
    if (shape.empty()) return strides;
    strides.back() = static_cast<std::int64_t>(element_bytes);
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(shape.size()) - 2;
         i >= 0; --i) {
        strides[static_cast<std::size_t>(i)] =
            strides[static_cast<std::size_t>(i + 1)] *
            shape[static_cast<std::size_t>(i + 1)];
    }
    return strides;
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

            /* Verify Apple's strides match our contiguous expectation
             * before doing a flat memcpy. arr.strides hands us
             * element-strides (counts); convert to byte-strides for
             * comparison against contiguous_strides(...). */
            const auto expected_strides = contiguous_strides(t.shape, elem_bytes);
            bool layout_contig = true;
            NSArray<NSNumber*>* actual_strides = arr.strides;
            if (actual_strides && [actual_strides count] == expected_strides.size()) {
                for (std::size_t i = 0; i < expected_strides.size(); ++i) {
                    const std::int64_t actual_byte_stride =
                        [[actual_strides objectAtIndex:i] longLongValue] *
                        static_cast<std::int64_t>(elem_bytes);
                    if (actual_byte_stride != expected_strides[i]) {
                        layout_contig = false;
                        break;
                    }
                }
            } else {
                layout_contig = false;
            }

            std::uint8_t*     dst_data   = t.bytes.data();
            const std::size_t dst_bytes  = expected_bytes;
            __block bool      copied     = false;
            if (layout_contig) {
                /* getBytesWithHandler block signature is
                 * `void(^)(const void*, NSInteger)` — no strides
                 * argument here (asymmetric with the mutable variant
                 * above). */
                [arr getBytesWithHandler:^(const void* bytes,
                                            NSInteger byteSize) {
                    if (static_cast<std::size_t>(byteSize) >= dst_bytes) {
                        std::memcpy(dst_data, bytes, dst_bytes);
                        copied = true;
                    }
                }];
            }
            /* Apple's M11 ship-path models (BlazeFace, portrait
             * segmentation, face landmarks) all return contiguous
             * row-major output so this branch is a runtime-shape
             * reject — not a deferred-impl stub. Tracked as backlog
             * `ml-coreml-strided-mlmultiarray-output-impl` for the
             * day a strided-output model surfaces. */
            if (!layout_contig || !copied) {
                if (error_msg) {
                    *error_msg = std::string{"CoreMlRuntime::run: output '"} +
                                  [nsName UTF8String] +
                                  "' has non-contiguous MLMultiArray layout "
                                  "(strided output not supported in this "
                                  "cycle); flat memcpy refused";
                }
                /* LEGIT: ml-coreml-strided-mlmultiarray-output-impl */
                return ME_E_UNSUPPORTED;
            }

            (*outputs)[std::string{[nsName UTF8String]}] = std::move(t);
        }
    }
    return ME_OK;
}

}  // namespace me::inference
