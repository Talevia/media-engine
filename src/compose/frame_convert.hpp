/*
 * me::compose::frame_convert — libswscale wrappers for AVFrame ↔ RGBA8
 * conversions that the compose path needs.
 *
 * Sits between the decoder output (arbitrary YUV / etc. AVFrame from
 * libavcodec) and the `me::compose::alpha_over` kernel which wants
 * tightly-packed non-premultiplied RGBA8 buffers. Placed in
 * `src/compose/` (not `src/io/`) because these conversions are
 * compose-scope — reencode-single-track doesn't need them (it stays
 * in YUV end-to-end).
 *
 * Contracts:
 *   - `frame_to_rgba8`: src AVFrame → `std::vector<uint8_t>` sized to
 *     `width * height * 4`, row stride == width * 4. Alpha channel
 *     defaults to 255 for source formats without alpha (every common
 *     decode output today).
 *   - `rgba8_to_frame`: tightly-packed RGBA8 → pre-allocated dst
 *     AVFrame (caller sets format / width / height / data / linesize;
 *     this just runs `sws_scale`).
 *
 * Both pick `SWS_BILINEAR` scaling filter — identity when src and dst
 * dimensions match (which is the common compose-path case). For the
 * byte-deterministic software path the key property is no
 * `SWS_FAST_BILINEAR` (which can vary with SIMD availability), no
 * threading, no runtime-dispatch filter changes.
 */
#pragma once

#include "media_engine/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVFrame;

namespace me::compose {

/* Convert an AVFrame (any libav pixel format sws can handle) to
 * tightly-packed RGBA8. Output resized to width*height*4.
 *   - src null / invalid → ME_E_INVALID_ARG.
 *   - sws_getContext / sws_scale failure → ME_E_INTERNAL.
 *   - success → ME_OK, dst_buf.size() == src->width * src->height * 4. */
me_status_t frame_to_rgba8(const AVFrame*        src,
                            std::vector<uint8_t>& dst_buf,
                            std::string*          err = nullptr);

/* Convert tightly-packed RGBA8 (stride_bytes = width * 4) into a
 * pre-allocated dst AVFrame. Caller sets dst->format (e.g.
 * AV_PIX_FMT_YUV420P / YUV444P / NV12), dst->width, dst->height, and
 * allocates dst->data / dst->linesize (typically via av_frame_get_buffer
 * + av_image_fill_arrays).
 *   - src / dst null → ME_E_INVALID_ARG.
 *   - dimension mismatch (dst width/height != width/height arg) → ME_E_INVALID_ARG.
 *   - sws failure → ME_E_INTERNAL. */
me_status_t rgba8_to_frame(const uint8_t* src,
                            int            width,
                            int            height,
                            std::size_t    stride_bytes,
                            AVFrame*       dst,
                            std::string*   err = nullptr);

}  // namespace me::compose
