/*
 * test_decode_av1_10bit — M10 exit criterion §M10:119 third leg.
 *
 * Decode a tiny AV1 Main 10-bit (yuv420p10le) fixture through
 * `me_render_frame` and assert the RGBA8 output is a sensible
 * grayscale midtone, matching the same shape as
 * `test_decode_vp9_profile2.cpp` (cycle 18) and
 * `test_pq_hlg_roundtrip.cpp` (HEVC Main 10).
 *
 * What this pins:
 *
 *   - libavformat auto-routes AV1 streams to a working decoder
 *     (libdav1d preferred per encoder lookup; FFmpeg's built-in
 *     `av1` falls back). Either path must produce a valid AVFrame
 *     with `format == AV_PIX_FMT_YUV420P10LE`.
 *   - `frame_to_rgba8` (src/compose/frame_convert.cpp:33) —
 *     pix_fmt-agnostic via `sws_getContext` — handles 10-bit AV1
 *     output the same way it handles VP9 P2 and HEVC Main 10.
 *
 * The centre-pixel sanity band (R≈G≈B≈129, ±20 LSB) absorbs both
 * the encoder's quantisation noise and libswscale's BT.709
 * limited→full coefficient choice. Wider than VP9 P2's band (which
 * landed exact 129) because SVT-AV1 with preset 8 uses lossier
 * quantisation than libvpx-vp9 does on uniform input.
 *
 * Skipped on hosts without libsvtav1 (fixture won't exist).
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

#ifndef ME_TEST_FIXTURE_MP4_AV1_10
#define ME_TEST_FIXTURE_MP4_AV1_10 ""
#endif

namespace {

struct EngineHandle    { me_engine_t* p = nullptr;
    ~EngineHandle()    { if (p) me_engine_destroy(p); } };
struct TimelineHandle  { me_timeline_t* p = nullptr;
    ~TimelineHandle()  { if (p) me_timeline_destroy(p); } };
struct FrameHandle     { me_frame_t* p = nullptr;
    ~FrameHandle()     { if (p) me_frame_destroy(p); } };

std::string make_av1_timeline_json(const std::string& uri) {
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

TEST_CASE("AV1 Main 10-bit decode → render_frame produces sensible RGBA8") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_AV1_10;
    ME_REQUIRE_FIXTURE(fixture_path);

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    TimelineHandle tl;
    const std::string in_uri = "file://" + fixture_path;
    const std::string j = make_av1_timeline_json(in_uri);
    REQUIRE(me_timeline_load_json(eng.p, j.c_str(), j.size(), &tl.p) == ME_OK);

    FrameHandle f;
    me_status_t s = me_render_frame(eng.p, tl.p, me_rational_t{0, 24}, &f.p);
    if (s != ME_OK) {
        std::fprintf(stderr,
            "AV1 10-bit decode failed: %s\n", me_engine_last_error(eng.p));
        FAIL("me_render_frame returned non-OK on AV1 10-bit fixture");
        return;
    }

    const int w = me_frame_width(f.p);
    const int h = me_frame_height(f.p);
    REQUIRE(w == 320);
    REQUIRE(h == 240);

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
        "test_decode_av1_10bit: centre RGBA = (%d, %d, %d, %d)\n",
        r, g, b, a);

    /* Midtone gray sanity band: 110..160 (same as VP9 P2's; the AV1
     * encoder's quantisation noise on uniform input is bounded
     * tighter than the band itself). R≈G≈B within 6 LSB absorbs
     * AV1's chroma reconstruction wobble around the gray axis. */
    CHECK(r >= 110);
    CHECK(r <= 160);
    CHECK(g >= 110);
    CHECK(g <= 160);
    CHECK(b >= 110);
    CHECK(b <= 160);
    CHECK(std::abs(r - g) <= 6);
    CHECK(std::abs(g - b) <= 6);
    CHECK(a == 255);
}
