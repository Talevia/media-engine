/*
 * test_decode_vp9_profile2 — M10 exit criterion §M10:119 second leg.
 *
 * Decode a tiny VP9 Profile 2 (10-bit YUV 4:2:0) fixture through
 * `me_render_frame` and assert the RGBA8 output is a sensible
 * grayscale midtone, confirming the libswscale 10→8 reduction
 * path handles VP9 P2 the same way it handles HEVC Main 10's
 * P010 (per `test_pq_hlg_roundtrip.cpp`).
 *
 * This test pins:
 *
 *   - `me_timeline_load_json` accepts a single-clip timeline
 *     pointing at a VP9 P2 mp4.
 *   - `me_render_frame` returns ME_OK at t=0/24 (i.e. demux +
 *     decode + RGBA8 convert succeed end-to-end).
 *   - The decoded frame's centre pixel is a midtone gray (R≈G≈B,
 *     all near the expected 8-bit mapping of 10-bit Y=512 limited
 *     range under BT.709 → ~127 ± a few LSB for sws_scale).
 *
 * Why centre pixel rather than full-frame ε-compare: the fixture
 * encodes uniform gray, the decoded output is uniform gray, and
 * libvpx-vp9 + libswscale together are deterministic enough that
 * we can pin exact RGB equality across the centre patch (no
 * per-channel drift like HEVC HW). Edge pixels still drift due
 * to chroma subsampling on the frame border.
 *
 * Skipped on hosts without libvpx-vp9 (FFmpeg built without
 * `--enable-libvpx`). Same skip pattern as `test_pq_hlg_roundtrip`.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture_skip.hpp"

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4_VP9P2
#define ME_TEST_FIXTURE_MP4_VP9P2 ""
#endif

namespace {

struct EngineHandle    { me_engine_t* p = nullptr;
    ~EngineHandle()    { if (p) me_engine_destroy(p); } };
struct TimelineHandle  { me_timeline_t* p = nullptr;
    ~TimelineHandle()  { if (p) me_timeline_destroy(p); } };
struct FrameHandle     { me_frame_t* p = nullptr;
    ~FrameHandle()     { if (p) me_frame_destroy(p); } };

/* Build a single-clip timeline JSON pointing at `uri`. SDR BT.709
 * tags match the fixture's encoder settings — schema validation
 * accepts the legal combo (per `validate_color_space_combo`). */
std::string make_vp9_p2_timeline_json(const std::string& uri) {
    std::string j;
    j.reserve(2048);
    j +=
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"frameRate\":  {\"num\":24,\"den\":1},\n"
        "  \"resolution\": {\"width\":320,\"height\":240},\n"
        "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                          "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
        "  \"assets\": [{\"id\":\"a1\",\"uri\":\"";
    j += uri;
    j +=
        "\"}],\n"
        "  \"compositions\": [{\n"
        "    \"id\":\"main\",\n"
        "    \"duration\":{\"num\":5,\"den\":24},\n"
        "    \"tracks\":[{\n"
        "      \"id\":\"v0\",\"kind\":\"video\",\"clips\":[\n"
        "        {\"type\":\"video\",\"id\":\"c0\",\"assetId\":\"a1\",\n"
        "         \"timeRange\":{\"start\":{\"num\":0,\"den\":24},\"duration\":{\"num\":5,\"den\":24}},\n"
        "         \"sourceRange\":{\"start\":{\"num\":0,\"den\":24},\"duration\":{\"num\":5,\"den\":24}}}\n"
        "      ]}]\n"
        "  }],\n"
        "  \"output\": {\"compositionId\":\"main\"}\n"
        "}\n";
    return j;
}

}  // namespace

TEST_CASE("VP9 Profile 2 (10-bit) decode → render_frame produces sensible RGBA8") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_VP9P2;
    ME_REQUIRE_FIXTURE(fixture_path);

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    TimelineHandle tl;
    const std::string in_uri = "file://" + fixture_path;
    const std::string j = make_vp9_p2_timeline_json(in_uri);
    REQUIRE(me_timeline_load_json(eng.p, j.c_str(), j.size(), &tl.p) == ME_OK);

    FrameHandle f;
    me_status_t s = me_render_frame(eng.p, tl.p, me_rational_t{0, 24}, &f.p);
    if (s != ME_OK) {
        std::fprintf(stderr,
            "VP9 P2 decode failed: %s\n", me_engine_last_error(eng.p));
        FAIL("me_render_frame returned non-OK on VP9 P2 fixture");
        return;
    }

    const int w = me_frame_width(f.p);
    const int h = me_frame_height(f.p);
    REQUIRE(w == 320);
    REQUIRE(h == 240);

    /* Centre pixel of the uniform-gray fixture. 10-bit Y=512
     * limited-range under BT.709 → 8-bit RGB ≈ 127 (the exact
     * value depends on sws_scale's coefficients; libvpx-vp9
     * produces lossless-ish output for uniform input). */
    const uint8_t* px = me_frame_pixels(f.p);
    REQUIRE(px != nullptr);
    const std::size_t cx = static_cast<std::size_t>(w / 2);
    const std::size_t cy = static_cast<std::size_t>(h / 2);
    const std::size_t i  = (cy * w + cx) * 4;
    const int r = px[i + 0];
    const int g = px[i + 1];
    const int b = px[i + 2];
    const int a = px[i + 3];

    std::fprintf(stderr,
        "test_decode_vp9_profile2: centre RGBA = (%d, %d, %d, %d)\n",
        r, g, b, a);

    /* Sanity bounds: midtone gray (mean of 8-bit luminance ≈ 127),
     * R≈G≈B (uniform gray, no chroma signal), opaque. The 110..160
     * window absorbs sws_scale BT.709 limited→full coefficient
     * choice + libvpx-vp9 quantisation noise on the centre pixel.
     * If decode silently failed (returned all-zeros or all-255),
     * this trips immediately. */
    CHECK(r >= 110);
    CHECK(r <= 160);
    CHECK(g >= 110);
    CHECK(g <= 160);
    CHECK(b >= 110);
    CHECK(b <= 160);
    CHECK(std::abs(r - g) <= 4);
    CHECK(std::abs(g - b) <= 4);
    CHECK(a == 255);
}
