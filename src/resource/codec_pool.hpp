/*
 * CodecPool — engine-scoped tracker for AVCodecContext allocations.
 *
 * Phase-1 scope is narrow: track live AVCodecContext count per engine so
 * `me_cache_stats.codec_ctx_count` reports reality instead of zero. Actual
 * encoder reuse (the "pool" in the name) is deferred — FFmpeg encoders
 * generally aren't safe to reuse across streams without reopening, and
 * today there's no consumer that would benefit from pooling either.
 *
 * The allocated handle is a unique_ptr with a custom deleter that
 * (1) calls `avcodec_free_context` and (2) decrements the pool's live
 * counter. Callers that previously held
 * `me::io::AvCodecContextPtr` (from `io/ffmpeg_raii.hpp`) migrate to
 * `CodecPool::Ptr` — same RAII semantics, one extra pointer of state in
 * the deleter for the back-reference to the pool.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

struct AVCodec;
struct AVCodecContext;

namespace me::resource {

class CodecPool {
public:
    struct Deleter {
        CodecPool* pool = nullptr;
        void operator()(AVCodecContext* p) const noexcept;
    };
    using Ptr = std::unique_ptr<AVCodecContext, Deleter>;

    CodecPool() = default;
    CodecPool(const CodecPool&)            = delete;
    CodecPool& operator=(const CodecPool&) = delete;

    /* Allocate an AVCodecContext. On failure returns a null-owning Ptr
     * (still valid RAII-wise; caller checks `.get() == nullptr`). On
     * success increments `live_count()`; the returned Ptr's destructor
     * decrements it. */
    Ptr allocate(const AVCodec* codec);

    /* Number of AVCodecContexts currently owned by live Ptrs produced by
     * this pool. Monotonic-minus-decrement atomic; safe to query
     * concurrently with allocate/release. */
    std::int64_t live_count() const noexcept {
        return live_count_.load(std::memory_order_relaxed);
    }

private:
    friend struct Deleter;
    std::atomic<std::int64_t> live_count_{0};
};

}  // namespace me::resource
