/*
 * me::testing::ME_REQUIRE_FIXTURE — skip a doctest case when the
 * named filesystem path is missing or empty.
 *
 * Header-only; consolidates the same 5-line skip block that
 * previously lived inline in 16 test files (cycle 103 audit:
 * grep "skipping" across the tests/ tree returned the list).
 * Each call site repeated:
 *
 *   if (fixture_path.empty() || !fs::exists(fixture_path)) {
 *       MESSAGE("skipping: fixture not available");
 *       return;
 *   }
 *
 * This macro is the single source of truth. Use it as the first
 * statement inside any TEST_CASE that depends on a runtime
 * fixture file (typically ME_TEST_FIXTURE_MP4 from CMake or a
 * fs::path local). The macro `return`s the enclosing function on
 * skip — so it must be invoked at TEST_CASE / SUBCASE scope, not
 * from a helper that the test calls.
 */
#pragma once

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>

#define ME_REQUIRE_FIXTURE(path)                                           \
    do {                                                                   \
        const std::string _me_fixture_path_str{(path)};                    \
        if (_me_fixture_path_str.empty() ||                                \
            !std::filesystem::exists(_me_fixture_path_str)) {              \
            MESSAGE("skipping: fixture not available");                    \
            return;                                                        \
        }                                                                  \
    } while (0)
