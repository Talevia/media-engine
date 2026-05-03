/* prepare_blazeface_input impl. See header. */
#include "compose/blazeface_preprocess.hpp"

#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <cstring>
#include <vector>

namespace me::compose {

me_status_t prepare_blazeface_input(
    const std::uint8_t*    rgba,
    int                    width,
    int                    height,
    std::size_t            stride_bytes,
    me::inference::Tensor* out,
    std::string*           err) {
    if (!rgba || width <= 0 || height <= 0 || !out) return ME_E_INVALID_ARG;
    if (stride_bytes < static_cast<std::size_t>(width) * 4) return ME_E_INVALID_ARG;

    constexpr int N = kBlazefaceInputDim;

    /* Step 1: resize RGBA → 128×128 RGBA via libswscale. AREA
     * filter is the cheapest decimation kernel that's documented
     * across libswscale versions. (BICUBIC would also work but
     * costs more and the visual difference at 128px target is
     * negligible for face detection.) */
    me::io::SwsContextPtr sws(sws_getContext(
        width, height, AV_PIX_FMT_RGBA,
        N, N, AV_PIX_FMT_RGBA,
        SWS_AREA, nullptr, nullptr, nullptr));
    if (!sws) {
        if (err) *err = "prepare_blazeface_input: sws_getContext failed";
        return ME_E_INTERNAL;
    }

    std::vector<std::uint8_t> resized(static_cast<std::size_t>(N) * N * 4);
    const std::uint8_t* src_slices[1]  = { rgba };
    const int           src_strides[1] = { static_cast<int>(stride_bytes) };
    std::uint8_t*       dst_slices[1]  = { resized.data() };
    const int           dst_strides[1] = { N * 4 };

    const int sliced = sws_scale(sws.get(),
                                  src_slices, src_strides, 0, height,
                                  dst_slices, dst_strides);
    if (sliced != N) {
        if (err) *err = "prepare_blazeface_input: sws_scale produced "
                        "fewer than expected output rows";
        return ME_E_INTERNAL;
    }

    /* Step 2: RGBA → planar CHW float32, normalized [-1, 1].
     * Output layout: [R-plane (128*128 floats),
     *                 G-plane (128*128 floats),
     *                 B-plane (128*128 floats)]. Alpha
     * discarded. */
    out->shape = { 1, 3, N, N };
    out->dtype = me::inference::Dtype::Float32;
    out->bytes.assign(static_cast<std::size_t>(1) * 3 * N * N * 4, 0);

    auto* fp = reinterpret_cast<float*>(out->bytes.data());
    constexpr std::size_t plane_size = static_cast<std::size_t>(N) * N;

    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const std::size_t pix = (static_cast<std::size_t>(y) * N + x) * 4;
            const std::size_t idx = static_cast<std::size_t>(y) * N + x;
            fp[0 * plane_size + idx] =
                static_cast<float>(resized[pix + 0]) / 127.5f - 1.0f;
            fp[1 * plane_size + idx] =
                static_cast<float>(resized[pix + 1]) / 127.5f - 1.0f;
            fp[2 * plane_size + idx] =
                static_cast<float>(resized[pix + 2]) / 127.5f - 1.0f;
        }
    }
    return ME_OK;
}

}  // namespace me::compose
