#include "resource/frame_pool.hpp"

namespace me::resource {

namespace {

size_t bytes_per_pixel(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::Rgba8:    return 4;
        case PixelFormat::Rgba16F:  return 8;
        case PixelFormat::Nv12:     return 2;   /* approx for sizing */
        case PixelFormat::P010:     return 3;
        case PixelFormat::Yuv420p:  return 2;   /* approx */
    }
    return 4;
}

}  // namespace

std::shared_ptr<FrameHandle> FramePool::acquire(FrameSpec spec) {
    const size_t bytes = static_cast<size_t>(spec.width) *
                         static_cast<size_t>(spec.height) *
                         bytes_per_pixel(spec.fmt);

    /* Budget enforcement (budget_ == 0 means unlimited). The
     * comparison races against other concurrent acquires on this
     * atomic — two near-edge acquires may both see "fits" and
     * overshoot slightly. We accept that slop today; a real pool
     * with eviction lands later and will replace this gate. */
    if (budget_ > 0) {
        const int64_t held = used_.load(std::memory_order_acquire);
        if (held + static_cast<int64_t>(bytes) > budget_) {
            return nullptr;
        }
    }

    used_.fetch_add(static_cast<int64_t>(bytes), std::memory_order_release);
    acquisitions_.fetch_add(1, std::memory_order_relaxed);

    /* FrameHandle shared_ptr with a custom deleter that decrements
     * the pool's `used_` counter on destruction. Captures `this`
     * raw — safe under the pool-outlives-handles invariant
     * documented in frame_pool.hpp. The captured `bytes` avoids
     * re-deriving from spec on destruction. */
    FramePool* self = this;
    return std::shared_ptr<FrameHandle>(
        new FrameHandle(spec, std::vector<std::byte>(bytes, std::byte{0})),
        [self, bytes](FrameHandle* p) {
            self->used_.fetch_sub(static_cast<int64_t>(bytes),
                                   std::memory_order_release);
            delete p;
        });
}

float FramePool::pressure() const noexcept {
    if (budget_ <= 0) return 0.0f;
    const int64_t held = used_.load(std::memory_order_acquire);
    return static_cast<float>(held) / static_cast<float>(budget_);
}

FramePool::Stats FramePool::stats() const noexcept {
    return Stats{
        .memory_bytes_used  = used_.load(std::memory_order_relaxed),
        .memory_bytes_limit = budget_,
        .acquisitions       = acquisitions_.load(std::memory_order_relaxed),
    };
}

void FramePool::reset_counters() noexcept {
    /* Leave `used_` alone — it reflects currently-live FrameHandles
     * whose bytes return to the pool via the shared_ptr's custom
     * deleter. Only the cumulative-acquisitions debug counter is
     * resettable. */
    acquisitions_.store(0, std::memory_order_relaxed);
}

}  // namespace me::resource
