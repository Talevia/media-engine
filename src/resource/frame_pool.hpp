/*
 * FrameHandle / FramePool / CodecPool / GpuContext — resource ownership.
 *
 * Bootstrap scope: just enough API surface for kernels and scheduler to
 * hold references. Real pooling / budget / LRU eviction lands with the
 * `engine-owns-resources` backlog item.
 *
 * See docs/ARCHITECTURE_GRAPH.md §resource.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace me::resource {

enum class PixelFormat : uint8_t {
    Rgba8   = 0,
    Rgba16F = 1,
    Nv12    = 2,
    P010    = 3,
    Yuv420p = 4,
};

enum class Device : uint8_t {
    Cpu               = 0,
    GpuBgfx           = 1,
    HwDecoderSurface  = 2,
};

struct FrameSpec {
    int         width  = 0;
    int         height = 0;
    PixelFormat fmt    = PixelFormat::Rgba8;
    Device      dev    = Device::Cpu;
};

/* FrameHandle — refcounted via shared_ptr. Bootstrap holds CPU RGBA8 in a
 * vector<byte>; GPU/HW surface support lands with M3. */
class FrameHandle {
public:
    FrameHandle() = default;
    FrameHandle(FrameSpec spec, std::vector<std::byte> buf)
        : spec_(spec), buf_(std::move(buf)) {}

    const FrameSpec& spec() const noexcept { return spec_; }
    std::byte*       data()       noexcept { return buf_.data(); }
    const std::byte* data() const noexcept { return buf_.data(); }
    size_t           size() const noexcept { return buf_.size(); }

private:
    FrameSpec              spec_{};
    std::vector<std::byte> buf_;
};

/* FramePool — allocates FrameHandles with memory budget enforcement.
 *
 * `budget_bytes > 0` caps the total bytes of currently-held
 * FrameHandles; a ctor-time 0 means "unlimited" (no enforcement,
 * `pressure()` returns 0). On `acquire`, if a new frame would push
 * the current held total over the cap, the call returns `nullptr`
 * — no LRU eviction yet (tracked separately; this pool is still
 * the non-pooling variant — every acquire is a fresh allocation).
 *
 * `used_` semantic: "bytes of FrameHandles currently alive". The
 * shared_ptr returned by `acquire` carries a custom deleter that
 * decrements `used_` on destruction. Cumulative acquisitions (for
 * debugging / observability) stays in `acquisitions_`.
 *
 * Lifetime invariant: FramePool must outlive every FrameHandle
 * shared_ptr it emitted — the custom deleter reads `used_` at
 * destruction. The engine owns both via `me_engine::frames`
 * (engine_impl.hpp), which outlives anything that acquires frames
 * through its scheduler / orchestrators. Tests that want to drop
 * the pool early must first release their handles. */
class FramePool {
public:
    explicit FramePool(int64_t budget_bytes = 0) : budget_(budget_bytes) {}

    /* Returns nullptr when budget is set AND the new allocation
     * would exceed it. Callers treat nullptr as back-pressure —
     * either wait for in-flight handles to release, or fail the
     * upstream pipeline stage. */
    std::shared_ptr<FrameHandle> acquire(FrameSpec spec);

    /* Ratio of currently-held bytes to budget. Returns 0 when
     * budget == 0 (unlimited); callers comparing pressure() >
     * threshold get "never trigger" behaviour under unlimited
     * config, which matches expectations. */
    float pressure() const noexcept;

    struct Stats {
        int64_t memory_bytes_used  = 0;
        int64_t memory_bytes_limit = 0;
        int64_t acquisitions       = 0;
    };
    Stats stats() const noexcept;

    /* Reset acquisitions counter only. Currently-held bytes can't
     * be cleared this way — they come from live FrameHandles whose
     * lifetimes the pool doesn't control. Used by me_cache_clear
     * so the "lifetime acquisitions" signal returns to a fresh
     * baseline while the in-flight-held counter stays accurate. */
    void reset_counters() noexcept;

private:
    int64_t               budget_ = 0;
    std::atomic<int64_t>  used_{0};          /* currently-held bytes */
    std::atomic<int64_t>  acquisitions_{0};  /* cumulative acquires */
};

/* GpuContext — bootstrap empty class; methods added when backlog items
 * that need it land. CodecPool moved to its own header `codec_pool.hpp`
 * once it grew past empty. */
class GpuContext {};

}  // namespace me::resource
