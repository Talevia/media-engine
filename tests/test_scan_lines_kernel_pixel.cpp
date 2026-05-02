/*
 * test_scan_lines_kernel_pixel — pixel regression for the M12
 * §156 (2/5) scan-lines kernel.
 *
 * Coverage:
 *   - Argument-shape rejects.
 *   - darkness=0 → no-op.
 *   - line_height_px=2 phase 0 darkens even rows; odd rows
 *     untouched.
 *   - phase_offset_px=1 darkens odd rows.
 *   - darkness=1 + matching row → black; non-matching row
 *     unchanged.
 *   - Alpha never modified.
 *   - Determinism: same params on identical input produce
 *     identical bytes.
 */
#include <doctest/doctest.h>

#include "compose/scan_lines_kernel.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> make_solid(int w, int h, std::size_t stride,
                                       std::uint8_t r, std::uint8_t g,
                                       std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> rgba(stride * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * stride +
                                   static_cast<std::size_t>(x) * 4;
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = a;
        }
    }
    return rgba;
}

}  // namespace

TEST_CASE("apply_scan_lines_inplace: null buffer rejected") {
    me::ScanLinesEffectParams p;
    p.darkness = 0.5f;
    CHECK(me::compose::apply_scan_lines_inplace(nullptr, 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_scan_lines_inplace: out-of-range params rejected") {
    std::vector<std::uint8_t> buf(16 * 16 * 4);
    me::ScanLinesEffectParams p;
    p.darkness = std::nan("");
    CHECK(me::compose::apply_scan_lines_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p = me::ScanLinesEffectParams{};
    p.darkness = 0.5f;
    p.line_height_px = 0;
    CHECK(me::compose::apply_scan_lines_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p = me::ScanLinesEffectParams{};
    p.darkness = 0.5f;
    p.phase_offset_px = -1;
    CHECK(me::compose::apply_scan_lines_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);

    p = me::ScanLinesEffectParams{};
    p.darkness = 0.5f;
    p.phase_offset_px = 64;
    CHECK(me::compose::apply_scan_lines_inplace(buf.data(), 16, 16, 64, p)
          == ME_E_INVALID_ARG);
}

TEST_CASE("apply_scan_lines_inplace: darkness=0 → no-op") {
    auto buf = make_solid(8, 8, 32, 200, 100, 50, 255);
    const auto snapshot = buf;
    me::ScanLinesEffectParams p;
    REQUIRE(me::compose::apply_scan_lines_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);
    CHECK(buf == snapshot);
}

TEST_CASE("apply_scan_lines_inplace: line_height=2, phase=0 → even rows darkened, odd rows unchanged") {
    auto buf = make_solid(4, 4, 16, 200, 100, 50, 255);
    me::ScanLinesEffectParams p;
    p.line_height_px = 2;
    p.darkness = 1.0f;  /* full black on darkened rows */
    p.phase_offset_px = 0;
    REQUIRE(me::compose::apply_scan_lines_inplace(buf.data(), 4, 4, 16, p)
            == ME_OK);

    /* Row 0 (even): R=G=B=0, alpha unchanged. */
    for (int x = 0; x < 4; ++x) {
        const std::size_t i = 0 * 16 + x * 4;
        CHECK(buf[i + 0] == 0);
        CHECK(buf[i + 1] == 0);
        CHECK(buf[i + 2] == 0);
        CHECK(buf[i + 3] == 255);
    }
    /* Row 1 (odd): unchanged. */
    for (int x = 0; x < 4; ++x) {
        const std::size_t i = 1 * 16 + x * 4;
        CHECK(buf[i + 0] == 200);
        CHECK(buf[i + 1] == 100);
        CHECK(buf[i + 2] == 50);
        CHECK(buf[i + 3] == 255);
    }
    /* Row 2 (even): darkened. */
    for (int x = 0; x < 4; ++x) {
        const std::size_t i = 2 * 16 + x * 4;
        CHECK(buf[i + 0] == 0);
    }
    /* Row 3 (odd): unchanged. */
    for (int x = 0; x < 4; ++x) {
        const std::size_t i = 3 * 16 + x * 4;
        CHECK(buf[i + 0] == 200);
    }
}

TEST_CASE("apply_scan_lines_inplace: phase_offset=1 darkens odd rows") {
    auto buf = make_solid(4, 4, 16, 200, 100, 50, 255);
    me::ScanLinesEffectParams p;
    p.line_height_px = 2;
    p.darkness = 1.0f;
    p.phase_offset_px = 1;
    REQUIRE(me::compose::apply_scan_lines_inplace(buf.data(), 4, 4, 16, p)
            == ME_OK);

    /* Row 0 (even): unchanged (phase=1 → only y % 2 == 1 darkens). */
    CHECK(buf[0 * 16 + 0] == 200);
    /* Row 1 (odd): black. */
    CHECK(buf[1 * 16 + 0] == 0);
    /* Row 2 (even): unchanged. */
    CHECK(buf[2 * 16 + 0] == 200);
    /* Row 3 (odd): black. */
    CHECK(buf[3 * 16 + 0] == 0);
}

TEST_CASE("apply_scan_lines_inplace: darkness=0.5 halves channel values on matching rows") {
    auto buf = make_solid(2, 2, 8, 200, 100, 50, 255);
    me::ScanLinesEffectParams p;
    p.line_height_px = 2;
    p.darkness = 0.5f;
    REQUIRE(me::compose::apply_scan_lines_inplace(buf.data(), 2, 2, 8, p)
            == ME_OK);

    /* Row 0 (matching): R = round(200 * 128 / 255) ≈ 100,
     * G ≈ 50, B ≈ 25. The mult is round((1-0.5)*255 + 0.5)
     * = 128. (200 * 128 + 127) / 255 = 25727 / 255 = 100. */
    CHECK(buf[0] == 100);
    CHECK(buf[1] == 50);
    CHECK(buf[2] >= 24);  /* allow ±1 */
    CHECK(buf[2] <= 26);
    CHECK(buf[3] == 255);

    /* Row 1 (non-matching): unchanged. */
    CHECK(buf[8 + 0] == 200);
    CHECK(buf[8 + 1] == 100);
    CHECK(buf[8 + 2] == 50);
}

TEST_CASE("apply_scan_lines_inplace: alpha never modified") {
    auto buf = make_solid(8, 8, 32, 100, 100, 100, 77);
    me::ScanLinesEffectParams p;
    p.darkness = 1.0f;
    p.line_height_px = 1;  /* every row darkened */
    REQUIRE(me::compose::apply_scan_lines_inplace(buf.data(), 8, 8, 32, p)
            == ME_OK);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 32 +
                                   static_cast<std::size_t>(x) * 4;
            CHECK(buf[i + 3] == 77);
        }
    }
}

TEST_CASE("apply_scan_lines_inplace: determinism — repeated invocation produces identical bytes") {
    auto a = make_solid(16, 16, 64, 200, 100, 50, 255);
    auto b = make_solid(16, 16, 64, 200, 100, 50, 255);
    me::ScanLinesEffectParams p;
    p.line_height_px = 3; p.darkness = 0.7f; p.phase_offset_px = 1;
    REQUIRE(me::compose::apply_scan_lines_inplace(a.data(), 16, 16, 64, p)
            == ME_OK);
    REQUIRE(me::compose::apply_scan_lines_inplace(b.data(), 16, 16, 64, p)
            == ME_OK);
    CHECK(a == b);
}
