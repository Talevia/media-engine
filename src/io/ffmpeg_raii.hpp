/*
 * FFmpeg RAII wrappers — single source of truth for the unique_ptr deleters
 * we hand libav resources to.
 *
 * Replaces per-TU copies of the same deleter structs (previously duplicated
 * across src/api/thumbnail.cpp and src/orchestrator/reencode_pipeline.cpp).
 * Any new TU that owns `AVCodecContext` / `AVFrame` / `AVPacket` /
 * `SwsContext` / `SwrContext` should use these typedefs rather than
 * declaring local copies.
 *
 * Deleter semantics mirror the libav cleanup functions exactly:
 *   avcodec_free_context(&ptr)   — frees ctx and sets pointer to null
 *   av_frame_free(&ptr)          — same shape
 *   av_packet_free(&ptr)         — same shape
 *   sws_freeContext(ptr)         — by-value
 *   swr_free(&ptr)               — by-address
 *
 * Null-checks are defensive; the libav functions are documented
 * null-tolerant, but keeping the guards avoids surprise if a future libav
 * release tightens contracts.
 */
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>

namespace me::io {

struct AvCodecContextDel {
    void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); }
};
struct AvFrameDel {
    void operator()(AVFrame* p) const noexcept { if (p) av_frame_free(&p); }
};
struct AvPacketDel {
    void operator()(AVPacket* p) const noexcept { if (p) av_packet_free(&p); }
};
struct SwsContextDel {
    void operator()(SwsContext* p) const noexcept { if (p) sws_freeContext(p); }
};
struct SwrContextDel {
    void operator()(SwrContext* p) const noexcept { if (p) swr_free(&p); }
};

using AvCodecContextPtr = std::unique_ptr<AVCodecContext, AvCodecContextDel>;
using AvFramePtr        = std::unique_ptr<AVFrame,        AvFrameDel>;
using AvPacketPtr       = std::unique_ptr<AVPacket,       AvPacketDel>;
using SwsContextPtr     = std::unique_ptr<SwsContext,     SwsContextDel>;
using SwrContextPtr     = std::unique_ptr<SwrContext,     SwrContextDel>;

}  // namespace me::io
