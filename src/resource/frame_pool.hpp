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

/* FramePool — bootstrap stub. Allocates fresh on acquire; no LRU, no
 * budget enforcement. Upgrade in `engine-owns-resources`. */
class FramePool {
public:
    explicit FramePool(int64_t budget_bytes = 0) : budget_(budget_bytes) {}

    std::shared_ptr<FrameHandle> acquire(FrameSpec spec);
    float                        pressure() const noexcept { return 0.0f; }

    struct Stats {
        int64_t memory_bytes_used  = 0;
        int64_t memory_bytes_limit = 0;
        int64_t acquisitions       = 0;
    };
    Stats stats() const noexcept;

private:
    int64_t               budget_ = 0;
    std::atomic<int64_t>  used_{0};
    std::atomic<int64_t>  acquisitions_{0};
};

/* CodecPool / GpuContext — bootstrap empty classes. Methods added when
 * backlog items that need them land. */
class CodecPool  {};
class GpuContext {};

}  // namespace me::resource
