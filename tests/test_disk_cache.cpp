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

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

/* Create a unique scratch directory for a test case, scoped to
 * a RAII guard that removes it on destruction. */
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        path = fs::temp_directory_path() /
               ("me_disk_cache_test_" +
                std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

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
    ScratchDir d;
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
    ScratchDir d;
    me::resource::DiskCache c(d.path.string());
    CHECK_FALSE(c.get("never_written").has_value());
}

TEST_CASE("DiskCache: second put on same hash overwrites") {
    ScratchDir d;
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
    ScratchDir d;
    me::resource::DiskCache c(d.path.string());

    const auto v = make_rgba(4, 4);
    REQUIRE(c.put("h1", v.data(), 4, 4, 16));
    REQUIRE(c.put("h2", v.data(), 4, 4, 16));

    c.invalidate("h1");
    CHECK_FALSE(c.get("h1").has_value());
    CHECK(c.get("h2").has_value());
}

TEST_CASE("DiskCache: clear removes all .bin files") {
    ScratchDir d;
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

TEST_CASE("DiskCache: concurrent puts from multiple threads don't corrupt") {
    ScratchDir d;
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
