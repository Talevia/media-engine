#include "resource/codec_pool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace me::resource {

void CodecPool::Deleter::operator()(AVCodecContext* p) const noexcept {
    if (p) avcodec_free_context(&p);
    if (pool) pool->live_count_.fetch_sub(1, std::memory_order_relaxed);
}

CodecPool::Ptr CodecPool::allocate(const AVCodec* codec) {
    if (!codec) return Ptr(nullptr, Deleter{this});
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)   return Ptr(nullptr, Deleter{this});
    live_count_.fetch_add(1, std::memory_order_relaxed);
    return Ptr(ctx, Deleter{this});
}

}  // namespace me::resource
