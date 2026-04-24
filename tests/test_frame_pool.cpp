/*
 * test_frame_pool — FramePool budget enforcement + currently-held
 * accounting + pressure contract.
 *
 * Pre-cycle behavior: FramePool's `budget_bytes` ctor parameter was
 * stored but never checked; `pressure()` returned 0 always; `used_`
 * was cumulative (monotonically grew). The
 * debt-frame-pool-enforce-budget cycle fixes all three.
 *
 * This suite pins:
 *   - budget == 0 → unlimited (never returns nullptr; pressure
 *     always 0).
 *   - budget == N → N/bytes allocations succeed, next returns
 *     nullptr.
 *   - FrameHandle destruction frees the bytes back to the pool, so
 *     a subsequent acquire succeeds.
 *   - pressure() reflects `used / budget` in [0, 1] range.
 *   - reset_counters() zeros acquisitions but preserves
 *     currently-held bytes.
 */
#include <doctest/doctest.h>

#include "resource/frame_pool.hpp"

#include <cstdint>
#include <memory>
#include <vector>

using me::resource::Device;
using me::resource::FrameHandle;
using me::resource::FramePool;
using me::resource::FrameSpec;
using me::resource::PixelFormat;

namespace {

constexpr int64_t kFrameBytesRGBA8_100x100 = 100 * 100 * 4;  /* 40 000 */
FrameSpec rgba8_100() { return FrameSpec{100, 100, PixelFormat::Rgba8, Device::Cpu}; }

}  // namespace

TEST_CASE("FramePool: unlimited budget (ctor default) never rejects") {
    FramePool pool(0);
    std::vector<std::shared_ptr<FrameHandle>> held;
    for (int i = 0; i < 10; ++i) {
        auto h = pool.acquire(rgba8_100());
        REQUIRE(h != nullptr);
        held.push_back(h);
    }
    /* pressure() stays 0 under unlimited regardless of held load. */
    CHECK(pool.pressure() == doctest::Approx(0.0f));

    const auto s = pool.stats();
    CHECK(s.memory_bytes_used  == 10 * kFrameBytesRGBA8_100x100);
    CHECK(s.memory_bytes_limit == 0);
    CHECK(s.acquisitions       == 10);
}

TEST_CASE("FramePool: budget caps currently-held bytes") {
    /* Budget of ~2.5 frames worth — allows 2, rejects the 3rd. */
    const int64_t budget = kFrameBytesRGBA8_100x100 * 5 / 2;
    FramePool pool(budget);

    auto h1 = pool.acquire(rgba8_100());
    REQUIRE(h1 != nullptr);
    auto h2 = pool.acquire(rgba8_100());
    REQUIRE(h2 != nullptr);

    /* 3rd would push us to 120 000 bytes > 100 000 cap. */
    auto h3 = pool.acquire(rgba8_100());
    CHECK(h3 == nullptr);

    /* Held = 80 000; pressure = 0.8. */
    CHECK(pool.pressure() == doctest::Approx(0.8f));

    const auto s = pool.stats();
    CHECK(s.memory_bytes_used  == 2 * kFrameBytesRGBA8_100x100);
    CHECK(s.memory_bytes_limit == budget);
    /* Rejected acquire did NOT increment acquisitions — the gate
     * fires before the counter bump. */
    CHECK(s.acquisitions       == 2);
}

TEST_CASE("FramePool: release via shared_ptr dtor frees budget") {
    const int64_t budget = kFrameBytesRGBA8_100x100;  /* exactly 1 frame */
    FramePool pool(budget);

    auto h = pool.acquire(rgba8_100());
    REQUIRE(h != nullptr);
    CHECK(pool.pressure() == doctest::Approx(1.0f));

    /* 2nd rejected — full. */
    CHECK(pool.acquire(rgba8_100()) == nullptr);

    /* Drop the handle — deleter decrements used_. */
    h.reset();
    CHECK(pool.pressure() == doctest::Approx(0.0f));

    /* Now acquire succeeds again. */
    auto h2 = pool.acquire(rgba8_100());
    REQUIRE(h2 != nullptr);
    CHECK(pool.pressure() == doctest::Approx(1.0f));
}

TEST_CASE("FramePool::reset_counters zeros acquisitions, preserves used") {
    FramePool pool(kFrameBytesRGBA8_100x100 * 10);
    auto h = pool.acquire(rgba8_100());
    REQUIRE(h != nullptr);

    auto s = pool.stats();
    CHECK(s.acquisitions       == 1);
    CHECK(s.memory_bytes_used  == kFrameBytesRGBA8_100x100);

    pool.reset_counters();

    s = pool.stats();
    CHECK(s.acquisitions       == 0);
    /* used_ preserved — h is still alive, its bytes are genuinely
     * held. reset_counters() only clears the cumulative debug
     * counter. */
    CHECK(s.memory_bytes_used  == kFrameBytesRGBA8_100x100);

    /* Drop the handle → used_ returns to 0. */
    h.reset();
    CHECK(pool.stats().memory_bytes_used == 0);
}

TEST_CASE("FramePool: zero-sized spec (width or height 0) succeeds with 0 bytes") {
    FramePool pool(kFrameBytesRGBA8_100x100);
    FrameSpec zero{0, 100, PixelFormat::Rgba8, Device::Cpu};
    auto h = pool.acquire(zero);
    /* 0 bytes is always "fits", even at budget = full. */
    REQUIRE(h != nullptr);
    CHECK(h->size() == 0);
}
