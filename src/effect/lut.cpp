#include "effect/lut.hpp"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace me::effect {

namespace {

/* Return a view of `s` with leading whitespace stripped. */
std::string_view lstrip(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
        ++i;
    }
    return s.substr(i);
}

/* Consume one line (up to and including '\n' / '\r\n'). Return the
 * line content (without terminator) and advance `pos`. */
std::string_view next_line(std::string_view text, std::size_t& pos) {
    const std::size_t start = pos;
    while (pos < text.size() && text[pos] != '\n') ++pos;
    std::string_view line = text.substr(start, pos - start);
    if (pos < text.size()) ++pos;  // consume '\n'
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

/* Parse three whitespace-separated floats from `line`. Returns
 * true iff exactly three valid floats were found (trailing data
 * allowed only if whitespace / comment). */
bool parse_three_floats(std::string_view line, float out[3]) {
    /* Copy into a NUL-terminated buffer for strtof. Lines in .cube
     * files are short (<100 chars typically); keep a small stack
     * buffer + bail on overflow. */
    char buf[256];
    if (line.size() >= sizeof(buf)) return false;
    std::memcpy(buf, line.data(), line.size());
    buf[line.size()] = '\0';

    char* p = buf;
    for (int i = 0; i < 3; ++i) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') return false;
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) return false;
        out[i] = v;
        p = end;
    }
    /* Trailing must be whitespace or empty. */
    while (*p != '\0') {
        if (*p != ' ' && *p != '\t' && *p != '\r') return false;
        ++p;
    }
    return true;
}

/* True iff line starts with (case-insensitive) `prefix`. */
bool starts_with_ci(std::string_view line, std::string_view prefix) {
    if (line.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(line[i])));
        const char b = static_cast<char>(std::tolower(
            static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

}  // namespace

CubeLut parse_cube_lut(std::string_view text) {
    CubeLut out;

    int         cube_size      = 0;
    std::size_t expected_cells = 0;

    std::size_t pos = 0;
    while (pos < text.size()) {
        std::string_view line = next_line(text, pos);
        line = lstrip(line);

        /* Skip empty + comment lines. */
        if (line.empty() || line[0] == '#') continue;

        if (starts_with_ci(line, "TITLE ") ||
            starts_with_ci(line, "DOMAIN_MIN ") ||
            starts_with_ci(line, "DOMAIN_MAX ")) {
            /* Accepted but ignored — domain outside [0, 1] is
             * uncommon; revisit when a real consumer needs it. */
            continue;
        }

        if (starts_with_ci(line, "LUT_1D_SIZE")) {
            /* 1D LUTs need a different shader path; reject loudly. */
            return CubeLut{};
        }
        if (starts_with_ci(line, "LUT_3D_INPUT_RANGE ")) {
            /* Same class as DOMAIN_*; ignore for phase-1. */
            continue;
        }

        if (starts_with_ci(line, "LUT_3D_SIZE")) {
            /* "LUT_3D_SIZE N" — one int after the keyword. */
            std::string_view rest = line.substr(
                std::strlen("LUT_3D_SIZE"));
            rest = lstrip(rest);
            char buf[32];
            if (rest.size() >= sizeof(buf)) return CubeLut{};
            std::memcpy(buf, rest.data(), rest.size());
            buf[rest.size()] = '\0';
            char* end = nullptr;
            const long n = std::strtol(buf, &end, 10);
            if (end == buf || n < 2 || n > 256) return CubeLut{};
            cube_size      = static_cast<int>(n);
            expected_cells = static_cast<std::size_t>(n) * n * n;
            out.rgb.reserve(expected_cells * 3);
            continue;
        }

        /* Otherwise must be an RGB triple. If we haven't seen
         * LUT_3D_SIZE yet, that's an error — can't validate row
         * count without it. */
        if (cube_size == 0) return CubeLut{};

        float rgb[3];
        if (!parse_three_floats(line, rgb)) return CubeLut{};
        out.rgb.push_back(rgb[0]);
        out.rgb.push_back(rgb[1]);
        out.rgb.push_back(rgb[2]);
    }

    if (cube_size == 0) return CubeLut{};
    if (out.rgb.size() != expected_cells * 3) return CubeLut{};

    out.cube_size = cube_size;
    return out;
}

}  // namespace me::effect
