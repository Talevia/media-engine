/*
 * test_api_surface_budget — VISION §5.7-4 guard.
 *
 * Scans `include/media_engine.h` + `include/media_engine/*.h`, counts:
 *   - distinct `me_<lower>_...(` function-call-shaped tokens (= public
 *     C API entry points)
 *   - semicolon-terminated declarations inside `typedef struct { ... }`
 *     blocks (= public struct fields visible across the C ABI)
 * and asserts both numbers are ≤ the budget recorded in
 * `tools/api_surface_budget.txt`.
 *
 * Why a *budget* and not a *fixed match*: a fixed match would burn a
 * ctest run every time we add a legit accessor. The budget mode lets
 * small organic growth pass silently while still failing on a
 * surface-explosion regression (e.g., a Map<String,Float>-shaped
 * effect-param API getting accidentally re-introduced in a wide PR).
 *
 * Why scan source rather than ABI: a compiled `nm` view would be
 * authoritative but couples the test to build flavor (debug symbols,
 * dead-code elim). The headers are the contract surface — what hosts
 * see and link against — and what we promise to keep small.
 *
 * Comment-stripping is a single pass that handles `/* … *\/` (no
 * nesting per C) + `// …`. Strings/chars containing `/*`-shaped
 * tokens don't appear in our headers; if they ever do, switch to a
 * stateful tokenizer rather than tightening the regex.
 *
 * Paths are injected via compile definitions so the test runs from
 * any working directory.
 */
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_block = false;
    bool in_line  = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (in_block) {
            if (c == '*' && n == '/') {
                in_block = false;
                ++i;
            }
        } else if (in_line) {
            if (c == '\n') {
                in_line = false;
                out += c;
            }
        } else {
            if (c == '/' && n == '*') {
                in_block = true;
                ++i;
            } else if (c == '/' && n == '/') {
                in_line = true;
                ++i;
            } else {
                out += c;
            }
        }
    }
    return out;
}

struct Counts {
    int functions = 0;
    int fields    = 0;
};

Counts scan_public_headers(const fs::path& include_dir) {
    std::set<std::string> functions;
    int fields = 0;

    /* Match `me_<lower>...(` — function-call-shaped tokens. Excludes
     * typedefs (no `(` after) and macros that don't look like calls. */
    const std::regex fn_re{R"(\bme_[a-z][a-zA-Z_0-9]*\s*\()"};

    for (const auto& entry : fs::recursive_directory_iterator(include_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".h") continue;

        const std::string src     = read_file(entry.path());
        const std::string clean   = strip_comments(src);

        for (auto it = std::sregex_iterator(clean.begin(), clean.end(), fn_re);
             it != std::sregex_iterator(); ++it) {
            std::string m = it->str();
            const auto paren = m.find('(');
            if (paren != std::string::npos) m.resize(paren);
            /* trim trailing whitespace */
            while (!m.empty() && std::isspace(static_cast<unsigned char>(m.back())))
                m.pop_back();
            functions.insert(m);
        }

        /* Count `;`-terminated declarations inside `typedef struct
         * <name>? { ... }` blocks. No nesting expected in our
         * headers; bail out at the matching closing brace. */
        size_t pos = 0;
        while ((pos = clean.find("typedef struct", pos)) != std::string::npos) {
            const size_t open = clean.find('{', pos);
            if (open == std::string::npos) break;
            int depth = 1;
            size_t i  = open + 1;
            const size_t body_start = i;
            while (i < clean.size() && depth > 0) {
                if (clean[i] == '{') ++depth;
                else if (clean[i] == '}') --depth;
                ++i;
            }
            const size_t body_end = (depth == 0) ? (i - 1) : clean.size();
            const std::string body = clean.substr(body_start, body_end - body_start);
            for (char c : body) if (c == ';') ++fields;
            pos = i;
        }
    }
    return {static_cast<int>(functions.size()), fields};
}

struct Budget {
    int functions_max = 0;
    int fields_max    = 0;
};

Budget read_budget(const fs::path& budget_file) {
    Budget b;
    std::ifstream f(budget_file);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const int val = std::stoi(line.substr(eq + 1));
        if (key == "functions") b.functions_max = val;
        else if (key == "fields") b.fields_max = val;
    }
    return b;
}

}  // namespace

TEST_CASE("API surface budget — public me_ functions ≤ budget") {
    const fs::path inc = ME_TEST_INCLUDE_DIR;
    const fs::path bud = ME_TEST_API_BUDGET_FILE;
    REQUIRE(fs::exists(inc));
    REQUIRE(fs::exists(bud));

    const auto counts  = scan_public_headers(inc);
    const auto budget  = read_budget(bud);

    REQUIRE(budget.functions_max > 0);
    REQUIRE(budget.fields_max    > 0);

    INFO("public me_ functions: " << counts.functions
         << " (budget=" << budget.functions_max << ")");
    CHECK(counts.functions <= budget.functions_max);

    INFO("public struct fields: " << counts.fields
         << " (budget=" << budget.fields_max << ")");
    CHECK(counts.fields <= budget.fields_max);

    /* Smoke: surface should be non-empty. Catches a regression where
     * the include dir path is wrong and we silently scan zero files. */
    CHECK(counts.functions > 0);
    CHECK(counts.fields > 0);
}
