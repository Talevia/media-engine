/*
 * me::effect::parse_cube_lut — minimal Adobe/IRIDAS .cube file
 * parser.
 *
 * Scope: LUT_3D_SIZE + RGB-triple data rows are mandatory; TITLE
 * / DOMAIN_MIN / DOMAIN_MAX / comment lines are accepted but
 * ignored (domain assumed to be [0, 1], matching the vast
 * majority of .cube LUTs in circulation). Other directives
 * (1D LUTs — LUT_1D_SIZE — / INPUT_RANGE) trigger an error so
 * a silent mis-parse doesn't ship garbage data to the shader.
 *
 * Returns the typed CubeLut on success; `cube_size == 0` signals
 * parse failure. No error string is surfaced to the caller — if
 * you need diagnostics, plug in error reporting in a follow-up.
 * For the first consumer (LutEffect) the distinction between
 * "file missing", "malformed text", and "unsupported directive"
 * isn't actionable yet.
 */
#pragma once

#include <string_view>
#include <vector>

namespace me::effect {

struct CubeLut {
    int                cube_size = 0;       /* entries per axis; 0 = parse failure */
    std::vector<float> rgb;                 /* cube_size^3 * 3 floats, row-major */
};

/* Parse .cube file contents. Return value.cube_size == 0 on failure. */
CubeLut parse_cube_lut(std::string_view text);

}  // namespace me::effect
