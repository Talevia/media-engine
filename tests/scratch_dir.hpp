/*
 * me::testing::ScratchDir — RAII scratch directory under
 * std::filesystem::temp_directory_path().
 *
 * Header-only; consolidates the same pattern that previously
 * lived in three test files (test_disk_cache.cpp,
 * test_frame_server.cpp, test_frame_server_concurrent.cpp) —
 * each rolled its own dir-name builder using
 * `reinterpret_cast<uintptr_t>(this | &local)` for uniqueness
 * and a hand-written dtor for cleanup.
 *
 * Uniqueness strategy: per-process atomic counter + thread-id
 * hash + caller-supplied slug. Two ScratchDirs constructed in
 * the same TEST_CASE (or the same parameterized subcase) get
 * different paths, so parallel ctest workers never collide.
 *
 * Usage:
 *   me::testing::ScratchDir d{"my_test_slug"};
 *   me::resource::DiskCache c(d.path.string());
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace me::testing {

class ScratchDir {
public:
    /* Public to preserve drop-in parity with the previous
     * `struct { fs::path path; }` shape used by callers like
     * `d.path.string()` / `d.path / "README.txt"`. */
    std::filesystem::path path;

    explicit ScratchDir(std::string_view slug) {
        static std::atomic<std::uint64_t> seq{0};
        const std::uint64_t n = seq.fetch_add(1, std::memory_order_relaxed);
        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        path = std::filesystem::temp_directory_path() /
               ("me_test_" + std::string{slug} + "_" +
                std::to_string(tid) + "_" + std::to_string(n));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    ScratchDir(const ScratchDir&)            = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&)                 = delete;
    ScratchDir& operator=(ScratchDir&&)      = delete;
};

}  // namespace me::testing
