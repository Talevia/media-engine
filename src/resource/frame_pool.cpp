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
    /* Bootstrap: fresh allocation every time. */
    const size_t bytes = static_cast<size_t>(spec.width) *
                        static_cast<size_t>(spec.height) *
                        bytes_per_pixel(spec.fmt);
    std::vector<std::byte> buf(bytes, std::byte{0});
    used_.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
    acquisitions_.fetch_add(1, std::memory_order_relaxed);
    return std::make_shared<FrameHandle>(spec, std::move(buf));
}

FramePool::Stats FramePool::stats() const noexcept {
    return Stats{
        .memory_bytes_used  = used_.load(std::memory_order_relaxed),
        .memory_bytes_limit = budget_,
        .acquisitions       = acquisitions_.load(std::memory_order_relaxed),
    };
}

void FramePool::reset_counters() noexcept {
    used_.store(0, std::memory_order_relaxed);
    acquisitions_.store(0, std::memory_order_relaxed);
}

}  // namespace me::resource
