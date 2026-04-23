/*
 * me::compose::frame_convert impl. See frame_convert.hpp for contract.
 */
#include "compose/frame_convert.hpp"

#include "io/av_err.hpp"
#include "io/ffmpeg_raii.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace me::compose {

me_status_t frame_to_rgba8(const AVFrame*        src,
                            std::vector<uint8_t>& dst_buf,
                            std::string*          err) {
    if (!src || src->width <= 0 || src->height <= 0) {
        if (err) *err = "frame_to_rgba8: invalid src (null or non-positive dimensions)";
        return ME_E_INVALID_ARG;
    }

    const int w = src->width;
    const int h = src->height;

    /* sws_getContext takes src pix_fmt (AVFrame stores it as int). */
    const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(src->format);

    me::io::SwsContextPtr sws(sws_getContext(w, h, src_fmt,
                                              w, h, AV_PIX_FMT_RGBA,
                                              SWS_BILINEAR,
                                              nullptr, nullptr, nullptr));
    if (!sws) {
        if (err) *err = "frame_to_rgba8: sws_getContext returned null";
        return ME_E_INTERNAL;
    }

    dst_buf.assign(static_cast<std::size_t>(w) * h * 4, 0);
    uint8_t*   dst_data[4] = { dst_buf.data(), nullptr, nullptr, nullptr };
    const int  dst_stride[4] = { w * 4, 0, 0, 0 };

    const int rc = sws_scale(sws.get(),
                              src->data, src->linesize, 0, h,
                              dst_data, dst_stride);
    if (rc != h) {
        if (err) *err = "frame_to_rgba8: sws_scale wrote " + std::to_string(rc) +
                        " rows, expected " + std::to_string(h);
        dst_buf.clear();
        return ME_E_INTERNAL;
    }
    return ME_OK;
}

me_status_t rgba8_to_frame(const uint8_t* src,
                            int            width,
                            int            height,
                            std::size_t    stride_bytes,
                            AVFrame*       dst,
                            std::string*   err) {
    if (!src || !dst || width <= 0 || height <= 0) {
        if (err) *err = "rgba8_to_frame: invalid args (null pointer or non-positive dimensions)";
        return ME_E_INVALID_ARG;
    }
    if (dst->width != width || dst->height != height) {
        if (err) *err = "rgba8_to_frame: dst AVFrame dimensions (" +
                        std::to_string(dst->width) + "x" + std::to_string(dst->height) +
                        ") don't match src (" + std::to_string(width) + "x" +
                        std::to_string(height) + ")";
        return ME_E_INVALID_ARG;
    }
    if (stride_bytes < static_cast<std::size_t>(width) * 4) {
        if (err) *err = "rgba8_to_frame: stride_bytes (" + std::to_string(stride_bytes) +
                        ") smaller than width*4 (" + std::to_string(width * 4) + ")";
        return ME_E_INVALID_ARG;
    }

    const AVPixelFormat dst_fmt = static_cast<AVPixelFormat>(dst->format);
    me::io::SwsContextPtr sws(sws_getContext(width, height, AV_PIX_FMT_RGBA,
                                              width, height, dst_fmt,
                                              SWS_BILINEAR,
                                              nullptr, nullptr, nullptr));
    if (!sws) {
        if (err) *err = "rgba8_to_frame: sws_getContext returned null";
        return ME_E_INTERNAL;
    }

    const uint8_t* src_data[4] = { src, nullptr, nullptr, nullptr };
    const int src_stride[4] = { static_cast<int>(stride_bytes), 0, 0, 0 };

    const int rc = sws_scale(sws.get(),
                              src_data, src_stride, 0, height,
                              dst->data, dst->linesize);
    if (rc != height) {
        if (err) *err = "rgba8_to_frame: sws_scale wrote " + std::to_string(rc) +
                        " rows, expected " + std::to_string(height);
        return ME_E_INTERNAL;
    }
    return ME_OK;
}

}  // namespace me::compose
