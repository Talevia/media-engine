#include "inference/onnx_runtime.hpp"

#include <onnxruntime_cxx_api.h>

#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace me::inference {

namespace {

ONNXTensorElementDataType dtype_to_ort(Dtype d, bool* ok) {
    *ok = true;
    switch (d) {
    case Dtype::Float32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case Dtype::Int32:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    case Dtype::Uint8:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
    case Dtype::Float16: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    }
    *ok = false;
    return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
}

bool ort_to_dtype(ONNXTensorElementDataType t, Dtype* out) {
    switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:   *out = Dtype::Float32; return true;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:   *out = Dtype::Int32;   return true;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:   *out = Dtype::Uint8;   return true;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: *out = Dtype::Float16; return true;
    default: return false;
    }
}

}  // namespace

struct OnnxRuntime::Impl {
    /* Snapshot of the host-supplied model bytes — kept until the
     * first `run()` call, then handed to Ort::Session and
     * released. ONNX Runtime copies bytes into its own arena
     * during session construction, so we can drop the std::vector
     * once the session is built. */
    std::vector<std::uint8_t> blob;

    std::mutex                    mu;
    bool                          init_attempted = false;
    std::string                   init_error;

    /* Lifetime ordering matters: env outlives session. Both must
     * be destroyed before the global Ort allocator goes away
     * (which only happens at process exit, so we don't care). */
    std::unique_ptr<Ort::Env>     env;
    std::unique_ptr<Ort::Session> session;
};

OnnxRuntime::OnnxRuntime() : impl_(std::make_unique<Impl>()) {}
OnnxRuntime::~OnnxRuntime() = default;

std::unique_ptr<OnnxRuntime> OnnxRuntime::create(
    const std::uint8_t* bytes,
    std::size_t         size,
    std::string*        error_msg) {
    if (!bytes || size == 0) {
        if (error_msg) {
            *error_msg = "OnnxRuntime::create: empty model blob";
        }
        return nullptr;
    }
    auto rt = std::unique_ptr<OnnxRuntime>(new OnnxRuntime());
    rt->impl_->blob.assign(bytes, bytes + size);
    return rt;
}

me_status_t OnnxRuntime::run(
    const std::map<std::string, Tensor>& inputs,
    std::map<std::string, Tensor>*       outputs,
    std::string*                         error_msg) {

    if (!outputs) {
        if (error_msg) *error_msg = "OnnxRuntime::run: null outputs map";
        return ME_E_INVALID_ARG;
    }

    /* Lazy session-init under the per-runtime mutex. */
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->init_attempted) {
            impl_->init_attempted = true;
            try {
                impl_->env = std::make_unique<Ort::Env>(
                    ORT_LOGGING_LEVEL_WARNING, "media-engine-onnx");
                Ort::SessionOptions opts;
                /* Single-thread CPU execution — VISION §3.4
                 * carve-out lets ML inference be non-deterministic,
                 * but minimizing avoidable thread-scheduling
                 * variation is cheap. ORT_ENABLE_BASIC keeps the
                 * graph optimizer's safe rewrites (constant
                 * folding, redundant-op elimination) without the
                 * level-2/3 fusions that depend on hardware
                 * specifics. */
                opts.SetIntraOpNumThreads(1);
                opts.SetInterOpNumThreads(1);
                opts.SetGraphOptimizationLevel(
                    GraphOptimizationLevel::ORT_ENABLE_BASIC);
                impl_->session = std::make_unique<Ort::Session>(
                    *impl_->env,
                    impl_->blob.data(),
                    impl_->blob.size(),
                    opts);
                /* Bytes copied into ORT arena; release the
                 * caller-supplied buffer to keep memory tight. */
                impl_->blob.clear();
                impl_->blob.shrink_to_fit();
            } catch (const Ort::Exception& e) {
                impl_->init_error =
                    std::string{"OnnxRuntime: failed to load model: "} +
                    e.what();
                impl_->session.reset();
                impl_->env.reset();
            } catch (const std::exception& e) {
                impl_->init_error =
                    std::string{"OnnxRuntime: unexpected exception during "
                                 "session construction: "} +
                    e.what();
                impl_->session.reset();
                impl_->env.reset();
            }
        }
        if (!impl_->session) {
            if (error_msg) *error_msg = impl_->init_error;
            return ME_E_DECODE;
        }
    }

    try {
        Ort::AllocatorWithDefaultOptions allocator;

        /* Build the ORT input vectors. ORT::Value::CreateTensor in
         * the borrowing form (CreateTensor(mem_info, void* p, size,
         * shape, ndims, type)) keeps a pointer to caller-owned
         * memory — safe here because `inputs` outlives the Run()
         * call. */
        std::vector<Ort::Value>     ort_inputs;
        std::vector<const char*>    input_names_c;
        std::vector<std::string>    input_names_storage;
        ort_inputs.reserve(inputs.size());
        input_names_c.reserve(inputs.size());
        input_names_storage.reserve(inputs.size());

        for (const auto& [name, tensor] : inputs) {
            bool dt_ok = false;
            const ONNXTensorElementDataType ort_type =
                dtype_to_ort(tensor.dtype, &dt_ok);
            if (!dt_ok) {
                if (error_msg) {
                    *error_msg = "OnnxRuntime::run: unsupported dtype for "
                                  "input '" + name + "'";
                }
                return ME_E_INVALID_ARG;
            }
            std::int64_t elem_count = 1;
            for (std::int64_t d : tensor.shape) elem_count *= d;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(elem_count) *
                dtype_byte_size(tensor.dtype);
            if (tensor.bytes.size() != expected_bytes) {
                if (error_msg) {
                    *error_msg = "OnnxRuntime::run: input '" + name +
                                  "' bytes.size() does not match "
                                  "product(shape) * dtype_byte_size";
                }
                return ME_E_INVALID_ARG;
            }

            const auto mem_info = Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator, OrtMemTypeDefault);
            ort_inputs.emplace_back(Ort::Value::CreateTensor(
                mem_info,
                /* CreateTensor borrows the buffer. const_cast is
                 * unavoidable because the C API takes void* even
                 * for the read-only path. ORT does not write to
                 * input tensors. */
                const_cast<std::uint8_t*>(tensor.bytes.data()),
                tensor.bytes.size(),
                tensor.shape.data(),
                tensor.shape.size(),
                ort_type));

            input_names_storage.push_back(name);
            input_names_c.push_back(input_names_storage.back().c_str());
        }

        /* Output names — fetch from the session each Run() so the
         * runtime never invents an extra output the model didn't
         * declare. */
        std::vector<std::string>  output_names_storage;
        std::vector<const char*>  output_names_c;
        const std::size_t n_outputs = impl_->session->GetOutputCount();
        output_names_storage.reserve(n_outputs);
        output_names_c.reserve(n_outputs);
        for (std::size_t i = 0; i < n_outputs; ++i) {
            Ort::AllocatedStringPtr name_ptr =
                impl_->session->GetOutputNameAllocated(i, allocator);
            output_names_storage.emplace_back(name_ptr.get());
            output_names_c.push_back(output_names_storage.back().c_str());
        }

        std::vector<Ort::Value> ort_outputs = impl_->session->Run(
            Ort::RunOptions{nullptr},
            input_names_c.data(),
            ort_inputs.data(),  ort_inputs.size(),
            output_names_c.data(), n_outputs);

        outputs->clear();
        for (std::size_t i = 0; i < ort_outputs.size(); ++i) {
            Ort::Value& v = ort_outputs[i];
            if (!v.IsTensor()) continue;  /* skip sequence / map outputs */

            const auto type_info = v.GetTensorTypeAndShapeInfo();
            Dtype dtype;
            if (!ort_to_dtype(type_info.GetElementType(), &dtype)) continue;

            const std::vector<std::int64_t> shape_vec = type_info.GetShape();
            std::int64_t elem_count = 1;
            for (std::int64_t d : shape_vec) elem_count *= d;
            const std::size_t elem_bytes = dtype_byte_size(dtype);
            const std::size_t total_bytes =
                static_cast<std::size_t>(elem_count) * elem_bytes;

            Tensor t;
            t.dtype = dtype;
            t.shape = shape_vec;
            t.bytes.resize(total_bytes);
            /* GetTensorData returns a typed pointer into ORT-owned
             * contiguous memory. Cast to void* + memcpy keeps the
             * byte-buffer abstraction. */
            const void* src = v.GetTensorData<std::uint8_t>();
            std::memcpy(t.bytes.data(), src, total_bytes);

            (*outputs)[output_names_storage[i]] = std::move(t);
        }
    } catch (const Ort::Exception& e) {
        if (error_msg) {
            *error_msg = std::string{"OnnxRuntime::run: prediction failed: "} +
                          e.what();
        }
        return ME_E_DECODE;
    } catch (const std::exception& e) {
        if (error_msg) {
            *error_msg = std::string{"OnnxRuntime::run: unexpected "
                                      "exception: "} + e.what();
        }
        return ME_E_DECODE;
    }
    return ME_OK;
}

}  // namespace me::inference
