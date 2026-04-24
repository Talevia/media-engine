/*
 * test_lut_parser — unit tests for me::effect::parse_cube_lut.
 *
 * Platform-agnostic; no bgfx link.
 *
 * Coverage:
 *   - Happy path: 2×2×2 identity LUT with title + comments parses.
 *   - Empty / no-size / short-data / bad-float → cube_size == 0.
 *   - LUT_1D_SIZE rejected.
 *   - Comments + blank lines + DOMAIN_{MIN,MAX} ignored cleanly.
 */
#include <doctest/doctest.h>

#include "effect/lut.hpp"

#include <string>

namespace {

std::string identity_2x2x2() {
    return R"(TITLE "identity 2x2x2"
# comment line

LUT_3D_SIZE 2
DOMAIN_MIN 0 0 0
DOMAIN_MAX 1 1 1
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";
}

}  // namespace

TEST_CASE("parse_cube_lut: 2x2x2 identity LUT with title + comments") {
    auto lut = me::effect::parse_cube_lut(identity_2x2x2());
    REQUIRE(lut.cube_size == 2);
    REQUIRE(lut.rgb.size() == 2 * 2 * 2 * 3);
    /* Entry 0 = (0, 0, 0). */
    CHECK(lut.rgb[0] == doctest::Approx(0.0f));
    CHECK(lut.rgb[1] == doctest::Approx(0.0f));
    CHECK(lut.rgb[2] == doctest::Approx(0.0f));
    /* Last entry (7) = (1, 1, 1). */
    CHECK(lut.rgb[21] == doctest::Approx(1.0f));
    CHECK(lut.rgb[22] == doctest::Approx(1.0f));
    CHECK(lut.rgb[23] == doctest::Approx(1.0f));
}

TEST_CASE("parse_cube_lut: empty input returns cube_size=0") {
    auto lut = me::effect::parse_cube_lut("");
    CHECK(lut.cube_size == 0);
    CHECK(lut.rgb.empty());
}

TEST_CASE("parse_cube_lut: missing LUT_3D_SIZE returns cube_size=0") {
    auto lut = me::effect::parse_cube_lut("0 0 0\n1 1 1\n");
    CHECK(lut.cube_size == 0);
}

TEST_CASE("parse_cube_lut: wrong entry count returns cube_size=0") {
    /* 2x2x2 needs 8 entries; give only 2. */
    auto lut = me::effect::parse_cube_lut(
        "LUT_3D_SIZE 2\n0 0 0\n1 1 1\n");
    CHECK(lut.cube_size == 0);
}

TEST_CASE("parse_cube_lut: non-numeric data rejected") {
    auto lut = me::effect::parse_cube_lut(
        "LUT_3D_SIZE 2\nabc 0 0\n1 1 1\n0 1 0\n1 0 1\n0 0 1\n1 0 0\n0 1 1\n1 1 1\n");
    CHECK(lut.cube_size == 0);
}

TEST_CASE("parse_cube_lut: LUT_1D_SIZE rejected") {
    auto lut = me::effect::parse_cube_lut(
        "LUT_1D_SIZE 16\n0 0 0\n");
    CHECK(lut.cube_size == 0);
}

TEST_CASE("parse_cube_lut: cube_size out-of-range rejected") {
    auto lut1 = me::effect::parse_cube_lut("LUT_3D_SIZE 1\n0 0 0\n");
    CHECK(lut1.cube_size == 0);  // < 2

    auto lut_huge = me::effect::parse_cube_lut("LUT_3D_SIZE 999\n");
    CHECK(lut_huge.cube_size == 0);  // > 256
}

TEST_CASE("parse_cube_lut: Windows CRLF line endings tolerated") {
    const std::string crlf =
        "LUT_3D_SIZE 2\r\n"
        "0 0 0\r\n1 0 0\r\n0 1 0\r\n1 1 0\r\n"
        "0 0 1\r\n1 0 1\r\n0 1 1\r\n1 1 1\r\n";
    auto lut = me::effect::parse_cube_lut(crlf);
    CHECK(lut.cube_size == 2);
    CHECK(lut.rgb.size() == 8 * 3);
}
