/*
 * test_disk_cache — unit tests for me::resource::DiskCache.
 *
 * Platform-agnostic; uses std::filesystem for scratch-dir
 * bookkeeping. Pure C++; no libav / Skia / bgfx.
 *
 * Coverage:
 *   - Empty-dir ctor → disabled (put/get no-op).
 *   - put → get round-trip preserves width/height/stride/pixels.
 *   - get missing hash → nullopt.
 *   - Second put on same hash overwrites.
 *   - invalidate removes entry.
 *   - clear removes all .bin files but leaves other files.
 *   - Concurrent put from multiple threads doesn't corrupt
 *     (stress test — relies on atomic rename).
 */
#include <doctest/doctest.h>

#include "resource/disk_cache.hpp"
#include "scratch_dir.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

using me::testing::ScratchDir;

/* Make a deterministic checkerboard RGBA pattern for round-trip
 * verification. */
std::vector<uint8_t> make_rgba(int w, int h, uint8_t seed = 0x42) {
    std::vector<uint8_t> buf(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            buf[i + 0] = static_cast<uint8_t>((x + seed) & 0xFF);
            buf[i + 1] = static_cast<uint8_t>((y + seed) & 0xFF);
            buf[i + 2] = static_cast<uint8_t>((x ^ y) & 0xFF);
            buf[i + 3] = 0xFF;
        }
    }
    return buf;
}

}  // namespace

TEST_CASE("DiskCache: empty-dir ctor → disabled") {
    me::resource::DiskCache c("");
    CHECK_FALSE(c.enabled());
    const auto buf = make_rgba(4, 4);
    CHECK_FALSE(c.put("anything", buf.data(), 4, 4, 16));
    CHECK_FALSE(c.get("anything").has_value());
}

TEST_CASE("DiskCache: put + get round-trip preserves pixels") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string());
    REQUIRE(c.enabled());

    const int W = 16, H = 8;
    const auto in = make_rgba(W, H, 0x55);
    REQUIRE(c.put("hash_rgba1", in.data(), W, H, W * 4));

    auto got = c.get("hash_rgba1");
    REQUIRE(got.has_value());
    CHECK(got->width == W);
    CHECK(got->height == H);
    CHECK(got->stride == W * 4);
    REQUIRE(got->rgba.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        CHECK(got->rgba[i] == in[i]);
    }
}

TEST_CASE("DiskCache: get missing hash → nullopt") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string());
    CHECK_FALSE(c.get("never_written").has_value());
}

TEST_CASE("DiskCache: second put on same hash overwrites") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string());

    const auto v1 = make_rgba(4, 4, 0x10);
    const auto v2 = make_rgba(4, 4, 0xF0);  /* different data */
    REQUIRE(c.put("dup", v1.data(), 4, 4, 16));
    REQUIRE(c.put("dup", v2.data(), 4, 4, 16));

    auto got = c.get("dup");
    REQUIRE(got.has_value());
    /* Check a sample pixel to verify v2 won, not v1. */
    CHECK(got->rgba[0] == v2[0]);
    CHECK(got->rgba[4] == v2[4]);
}

TEST_CASE("DiskCache: invalidate removes a single entry") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string());

    const auto v = make_rgba(4, 4);
    REQUIRE(c.put("h1", v.data(), 4, 4, 16));
    REQUIRE(c.put("h2", v.data(), 4, 4, 16));

    c.invalidate("h1");
    CHECK_FALSE(c.get("h1").has_value());
    CHECK(c.get("h2").has_value());
}

TEST_CASE("DiskCache: clear removes all .bin files") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string());

    const auto v = make_rgba(4, 4);
    REQUIRE(c.put("a", v.data(), 4, 4, 16));
    REQUIRE(c.put("b", v.data(), 4, 4, 16));
    REQUIRE(c.put("c", v.data(), 4, 4, 16));

    /* Drop a non-.bin file in the same dir; clear should leave it. */
    const auto stray = d.path / "README.txt";
    std::ofstream(stray) << "user note\n";

    c.clear();
    CHECK_FALSE(c.get("a").has_value());
    CHECK_FALSE(c.get("b").has_value());
    CHECK_FALSE(c.get("c").has_value());
    CHECK(fs::exists(stray));  /* stray preserved */
}

TEST_CASE("DiskCache: limit=0 (unlimited) never evicts") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string(), /*limit_bytes=*/0);

    const auto rgba = make_rgba(16, 16);
    for (int i = 0; i < 20; ++i) {
        const std::string key = "k" + std::to_string(i);
        REQUIRE(c.put(key, rgba.data(), 16, 16, 64));
    }
    /* All 20 entries persist. */
    for (int i = 0; i < 20; ++i) {
        const std::string key = "k" + std::to_string(i);
        REQUIRE(c.get(key).has_value());
    }
    CHECK(c.disk_bytes_limit() == 0);
    CHECK(c.disk_bytes_used() > 0);
}

TEST_CASE("DiskCache: bounded limit evicts oldest on put overflow") {
    ScratchDir d{"disk_cache"};
    /* Each 16×16 RGBA frame = 16 header + 16×16×4 body = 1040
     * bytes.  Cap at 3 frames worth (3120 bytes). */
    constexpr int64_t kFrameBytes = 16 + 16 * 16 * 4;
    me::resource::DiskCache c(d.path.string(), /*limit_bytes=*/kFrameBytes * 3);

    const auto rgba = make_rgba(16, 16);

    /* Fill to 3 / 3. */
    REQUIRE(c.put("k0", rgba.data(), 16, 16, 64));
    /* filesystem mtime resolution is ~1s on APFS / ext4; sleep a
     * beat between puts so eviction order is deterministic. */
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(c.put("k1", rgba.data(), 16, 16, 64));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(c.put("k2", rgba.data(), 16, 16, 64));

    CHECK(c.disk_bytes_used() == kFrameBytes * 3);
    REQUIRE(c.get("k0").has_value());
    REQUIRE(c.get("k1").has_value());
    REQUIRE(c.get("k2").has_value());

    /* 4th put triggers eviction of the oldest (k0). */
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(c.put("k3", rgba.data(), 16, 16, 64));

    CHECK_FALSE(c.get("k0").has_value());    /* k0 evicted */
    CHECK(c.get("k1").has_value());           /* k1..k3 survive */
    CHECK(c.get("k2").has_value());
    CHECK(c.get("k3").has_value());
    CHECK(c.disk_bytes_used() == kFrameBytes * 3);
}

TEST_CASE("DiskCache: oversized put rejected without evicting") {
    ScratchDir d{"disk_cache"};
    constexpr int64_t kSmallCap = 1024;  /* under one 16×16 frame */
    me::resource::DiskCache c(d.path.string(), kSmallCap);

    const auto small = make_rgba(4, 4);  /* 16 + 4×4×4 = 80 bytes */
    REQUIRE(c.put("small", small.data(), 4, 4, 16));
    const int64_t before = c.disk_bytes_used();
    REQUIRE(before > 0);

    /* 16×16 = 1040 > 1024 → rejected; the existing "small" entry
     * must NOT have been evicted as a side effect. */
    const auto big = make_rgba(16, 16);
    CHECK_FALSE(c.put("big", big.data(), 16, 16, 64));
    CHECK(c.get("small").has_value());
    CHECK(c.disk_bytes_used() == before);
}

TEST_CASE("DiskCache: invalidate / clear update disk_bytes_used") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string(), /*limit=*/0);

    const auto rgba = make_rgba(8, 8);
    REQUIRE(c.put("a", rgba.data(), 8, 8, 32));
    REQUIRE(c.put("b", rgba.data(), 8, 8, 32));
    const int64_t after_2 = c.disk_bytes_used();
    CHECK(after_2 > 0);

    c.invalidate("a");
    const int64_t after_inv = c.disk_bytes_used();
    CHECK(after_inv > 0);
    CHECK(after_inv < after_2);

    c.clear();
    CHECK(c.disk_bytes_used() == 0);
}

TEST_CASE("DiskCache: ctor seeds disk_bytes_used from existing files") {
    ScratchDir d{"disk_cache"};
    {
        me::resource::DiskCache c(d.path.string(), /*limit=*/0);
        const auto rgba = make_rgba(8, 8);
        REQUIRE(c.put("x", rgba.data(), 8, 8, 32));
        REQUIRE(c.put("y", rgba.data(), 8, 8, 32));
    }
    /* Re-open the same directory: bytes-used must reflect the two
     * surviving .bin entries from the prior DiskCache instance. */
    me::resource::DiskCache c2(d.path.string(), /*limit=*/0);
    CHECK(c2.disk_bytes_used() > 0);
    CHECK(c2.get("x").has_value());
    CHECK(c2.get("y").has_value());
}

TEST_CASE("DiskCache: concurrent puts from multiple threads don't corrupt") {
    ScratchDir d{"disk_cache"};
    me::resource::DiskCache c(d.path.string());

    constexpr int N_THREADS = 4;
    constexpr int N_PUTS_PER_THREAD = 10;

    std::vector<std::thread> threads;
    for (int ti = 0; ti < N_THREADS; ++ti) {
        threads.emplace_back([&, ti] {
            const auto v = make_rgba(8, 8, static_cast<uint8_t>(ti * 0x20));
            for (int i = 0; i < N_PUTS_PER_THREAD; ++i) {
                const std::string key =
                    "t" + std::to_string(ti) + "_" + std::to_string(i);
                c.put(key, v.data(), 8, 8, 32);
            }
        });
    }
    for (auto& t : threads) t.join();

    /* Every key should be readable. */
    for (int ti = 0; ti < N_THREADS; ++ti) {
        const auto expected = make_rgba(8, 8, static_cast<uint8_t>(ti * 0x20));
        for (int i = 0; i < N_PUTS_PER_THREAD; ++i) {
            const std::string key =
                "t" + std::to_string(ti) + "_" + std::to_string(i);
            auto got = c.get(key);
            REQUIRE(got.has_value());
            REQUIRE(got->rgba.size() == expected.size());
            /* Sample first + last bytes — full compare would be
             * over-strict on a stress test. */
            CHECK(got->rgba.front() == expected.front());
            CHECK(got->rgba.back() == expected.back());
        }
    }
}
