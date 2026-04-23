/*
 * test_thumbnail — me_thumbnail_png regression coverage.
 *
 * Before this suite existed, the PNG encode path shipped by
 * thumbnail-impl was only covered via the 06_thumbnail example — visual
 * "did you get a file out?" check with no CI tripwire for silent
 * regressions. Future refactors of the decoder seek, sws conversion, or
 * PNG muxer could corrupt output bytes or dimensions without any test
 * surfacing the drift.
 *
 * Fixture reuse: determinism_input.mp4 (gen_fixture output, 640×480 @
 * 25fps) is a known-dimension source so dimension assertions are stable
 * across environments. PNG magic + IHDR parsing is enough to
 * semantically validate "is this really a PNG with the right size?" —
 * we deliberately don't pixel-compare because the internal sws_scale +
 * PNG encoder output depends on libswscale filter defaults, which shift
 * subtly between FFmpeg versions. Dimension + magic is stable.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

namespace {

struct EngineHandle {
    me_engine_t* p = nullptr;
    ~EngineHandle() { if (p) me_engine_destroy(p); }
};

struct PngBuffer {
    uint8_t* data = nullptr;
    size_t   size = 0;
    ~PngBuffer() { if (data) me_buffer_free(data); }
};

/* PNG signature: 89 50 4E 47 0D 0A 1A 0A — first 8 bytes of any valid PNG.
 * Fixed by the spec, doesn't vary by encoder. */
constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/* PNG structure after the 8-byte signature is a sequence of chunks, each
 * 4 bytes length + 4 bytes type + payload + 4 bytes CRC. The very first
 * chunk MUST be IHDR, whose payload starts with 4 bytes width + 4 bytes
 * height in network byte order. That's the first 16 bytes after the
 * signature: 00 00 00 0D (len=13) + 49 48 44 52 ("IHDR") + W×H + ...
 *
 * Parse without pulling in libpng / any PNG library — we own the bytes
 * and the spec is fixed. */
struct PngHeader {
    int width  = -1;
    int height = -1;
};

PngHeader parse_png_header(const uint8_t* data, size_t size) {
    PngHeader h;
    if (size < 24) return h;
    if (std::memcmp(data, kPngSignature, 8) != 0) return h;
    if (std::memcmp(data + 12, "IHDR", 4) != 0) return h;
    auto rd_u32 = [&](size_t off) {
        return (static_cast<uint32_t>(data[off])     << 24) |
               (static_cast<uint32_t>(data[off + 1]) << 16) |
               (static_cast<uint32_t>(data[off + 2]) <<  8) |
               (static_cast<uint32_t>(data[off + 3]));
    };
    h.width  = static_cast<int>(rd_u32(16));
    h.height = static_cast<int>(rd_u32(20));
    return h;
}

}  // namespace

TEST_CASE("me_thumbnail_png produces native-dimension PNG from fixture") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping thumbnail test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    PngBuffer png;
    const std::string uri = "file://" + fixture_path;
    /* max_width=0, max_height=0 → native dimensions (640×480 per gen_fixture). */
    const me_status_t s = me_thumbnail_png(eng.p, uri.c_str(),
                                            me_rational_t{0, 1},
                                            0, 0,
                                            &png.data, &png.size);
    REQUIRE(s == ME_OK);
    REQUIRE(png.data != nullptr);
    REQUIRE(png.size > 24);   /* at least signature + IHDR chunk header */

    CHECK(std::memcmp(png.data, kPngSignature, 8) == 0);
    const PngHeader h = parse_png_header(png.data, png.size);
    CHECK(h.width  == 640);
    CHECK(h.height == 480);
}

TEST_CASE("me_thumbnail_png scales to max_width bound, preserving aspect ratio") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    PngBuffer png;
    /* max_width=320 on a 640×480 source → 320×240 (aspect preserved). */
    REQUIRE(me_thumbnail_png(eng.p, ("file://" + fixture_path).c_str(),
                              me_rational_t{0, 1}, 320, 0,
                              &png.data, &png.size) == ME_OK);
    const PngHeader h = parse_png_header(png.data, png.size);
    CHECK(h.width  == 320);
    CHECK(h.height == 240);
}

TEST_CASE("me_thumbnail_png scales to max_height bound, preserving aspect ratio") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    PngBuffer png;
    /* max_height=120 on a 640×480 source → 160×120 (aspect preserved). */
    REQUIRE(me_thumbnail_png(eng.p, ("file://" + fixture_path).c_str(),
                              me_rational_t{0, 1}, 0, 120,
                              &png.data, &png.size) == ME_OK);
    const PngHeader h = parse_png_header(png.data, png.size);
    CHECK(h.width  == 160);
    CHECK(h.height == 120);
}

TEST_CASE("me_thumbnail_png does not upscale past native dimensions") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    PngBuffer png;
    /* Bounding box larger than native → output stays at native. */
    REQUIRE(me_thumbnail_png(eng.p, ("file://" + fixture_path).c_str(),
                              me_rational_t{0, 1}, 1280, 1024,
                              &png.data, &png.size) == ME_OK);
    const PngHeader h = parse_png_header(png.data, png.size);
    CHECK(h.width  == 640);
    CHECK(h.height == 480);
}

TEST_CASE("me_thumbnail_png returns ME_E_IO for a non-existent URI") {
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    uint8_t* data = nullptr;
    size_t   size = 0;
    const me_status_t s = me_thumbnail_png(eng.p,
                                            "file:///nonexistent/not-a-file.mp4",
                                            me_rational_t{0, 1},
                                            0, 0,
                                            &data, &size);
    CHECK(s == ME_E_IO);
    CHECK(data == nullptr);
    CHECK(size == 0);

    const char* le = me_engine_last_error(eng.p);
    REQUIRE(le != nullptr);
    CHECK(std::string{le}.find("avformat_open_input") != std::string::npos);
}

TEST_CASE("me_thumbnail_png rejects null arguments with ME_E_INVALID_ARG") {
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    uint8_t* data = nullptr;
    size_t   size = 0;
    CHECK(me_thumbnail_png(nullptr,  "file:///x.mp4", me_rational_t{0,1}, 0, 0, &data, &size) == ME_E_INVALID_ARG);
    CHECK(me_thumbnail_png(eng.p,    nullptr,         me_rational_t{0,1}, 0, 0, &data, &size) == ME_E_INVALID_ARG);
    CHECK(me_thumbnail_png(eng.p,    "file:///x.mp4", me_rational_t{0,1}, 0, 0, nullptr, &size) == ME_E_INVALID_ARG);
    CHECK(me_thumbnail_png(eng.p,    "file:///x.mp4", me_rational_t{0,1}, 0, 0, &data, nullptr) == ME_E_INVALID_ARG);
}
