/* inference_input impl. See header. */
#include "compose/inference_input.hpp"

#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <cstring>
#include <vector>

namespace me::compose {

me_status_t prepare_inference_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    int                    target_w,
    int                    target_h,
    float                  scale,
    float                  bias,
    me::inference::Tensor* out,
    std::string*           err) {
    if (!rgba || width <= 0 || height <= 0 || !out) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;
    if (target_w <= 0 || target_h <= 0) return ME_E_INVALID_ARG;

    /* Step 1: resize RGBA → target_w × target_h RGBA via
     * libswscale. AREA filter is the cheapest documented
     * decimation kernel; spatial filtering above this resolution
     * doesn't change downstream model output enough to matter. */
    me::io::SwsContextPtr sws(sws_getContext(
        width, height, AV_PIX_FMT_RGBA,
        target_w, target_h, AV_PIX_FMT_RGBA,
        SWS_AREA, nullptr, nullptr, nullptr));
    if (!sws) {
        if (err) *err = "prepare_inference_input: sws_getContext failed";
        return ME_E_INTERNAL;
    }

    std::vector<std::uint8_t> resized(
        static_cast<std::size_t>(target_w) * target_h * 4);
    const std::uint8_t* src_slices[1]  = { rgba };
    const int           src_strides[1] = { static_cast<int>(stride_bytes) };
    std::uint8_t*       dst_slices[1]  = { resized.data() };
    const int           dst_strides[1] = { target_w * 4 };

    const int sliced = sws_scale(sws.get(),
                                  src_slices, src_strides, 0, height,
                                  dst_slices, dst_strides);
    if (sliced != target_h) {
        if (err) *err = "prepare_inference_input: sws_scale produced "
                        "fewer than expected output rows";
        return ME_E_INTERNAL;
    }

    /* Step 2: RGBA → planar CHW float32. Per-channel
     * normalization is `byte * scale + bias`. Alpha discarded. */
    out->shape = { 1, 3, target_h, target_w };
    out->dtype = me::inference::Dtype::Float32;
    out->bytes.assign(
        static_cast<std::size_t>(1) * 3 * target_h * target_w * 4, 0);

    auto* fp = reinterpret_cast<float*>(out->bytes.data());
    const std::size_t plane_size =
        static_cast<std::size_t>(target_w) * target_h;

    for (int y = 0; y < target_h; ++y) {
        for (int x = 0; x < target_w; ++x) {
            const std::size_t pix = (static_cast<std::size_t>(y) * target_w + x) * 4;
            const std::size_t idx = static_cast<std::size_t>(y) * target_w + x;
            fp[0 * plane_size + idx] =
                static_cast<float>(resized[pix + 0]) * scale + bias;
            fp[1 * plane_size + idx] =
                static_cast<float>(resized[pix + 1]) * scale + bias;
            fp[2 * plane_size + idx] =
                static_cast<float>(resized[pix + 2]) * scale + bias;
        }
    }
    return ME_OK;
}

me_status_t prepare_blazeface_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    me::inference::Tensor* out,
    std::string*           err) {
    /* BlazeFace: 128×128, normalize byte/127.5 - 1 → [-1, 1]. */
    return prepare_inference_input(
        rgba, width, height, stride_bytes,
        kBlazefaceInputDim, kBlazefaceInputDim,
        1.0f / 127.5f, -1.0f,
        out, err);
}

me_status_t prepare_selfie_segmentation_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    me::inference::Tensor* out,
    std::string*           err) {
    /* SelfieSegmentation: 256×256, normalize byte/255 → [0, 1]. */
    return prepare_inference_input(
        rgba, width, height, stride_bytes,
        kSelfieSegmentationInputDim, kSelfieSegmentationInputDim,
        1.0f / 255.0f, 0.0f,
        out, err);
}

}  // namespace me::compose
