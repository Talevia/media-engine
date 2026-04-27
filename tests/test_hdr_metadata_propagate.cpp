/*
 * test_hdr_metadata_propagate — M10 exit criterion 8 first half.
 *
 * Drives the full HDR side-data round-trip through the re-encode
 * path: probe HDR fixture → load timeline that references it →
 * me_render_start with `video_codec="hevc"` (HEVC re-encode via
 * hevc_videotoolbox, the M10 ship-path landed cycle 11) →
 * re-probe output → assert every HDR metadata field round-trips.
 *
 * What this pins (cycle 11 commit body promised this cycle would
 * close the loop):
 *
 *   - Encoder context propagates color_primaries / color_trc /
 *     colorspace / color_range from the source decoder (already
 *     in cycle 11's `open_video_encoder`).
 *
 *   - Side-data attachment (cycle 16): MasteringDisplayMetadata
 *     + ContentLightMetadata copied from source codecpar to
 *     output codecpar before `avformat_write_header`. Without
 *     that step, mp4 muxer drops `mdcv` / `clli` boxes on
 *     re-encode (passthrough preserves them via packet copy;
 *     re-encode wouldn't otherwise).
 *
 * Failure modes the test catches:
 *
 *   - Encoder strips the color tags from the output stream.
 *   - Side-data copy regression (e.g. someone refactors
 *     encoder_mux_setup.cpp and drops the loop).
 *   - mp4 muxer's mdcv/clli serialisation breaks (would also
 *     surface in `bench_hdr_roundtrip` Mode A's passthrough
 *     check, but this test exercises the re-encode path
 *     specifically).
 *
 * Skipped on hosts without VideoToolbox (HDR fixture won't exist
 * or hevc_videotoolbox encoder is absent). Same shape as
 * test_encode_hevc_main10's runtime skip.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include "fixture_skip.hpp"

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4_HDR
#define ME_TEST_FIXTURE_MP4_HDR ""
#endif

namespace {

struct EngineHandle {
    me_engine_t* p = nullptr;
    ~EngineHandle() { if (p) me_engine_destroy(p); }
};
struct InfoHandle {
    me_media_info_t* p = nullptr;
    ~InfoHandle() { if (p) me_media_info_destroy(p); }
};
struct TimelineHandle {
    me_timeline_t* p = nullptr;
    ~TimelineHandle() { if (p) me_timeline_destroy(p); }
};
struct RenderJobHandle {
    me_render_job_t* p = nullptr;
    ~RenderJobHandle() { if (p) me_render_job_destroy(p); }
};

bool rat_eq(me_rational_t a, int64_t num, int64_t den) {
    return a.num * den == num * a.den;
}
bool rat_eq_rt(me_rational_t a, me_rational_t b) {
    /* a == b ⇔ a.num*b.den == b.num*a.den. Same cross-multiply
     * helper bench_hdr_roundtrip + test_probe use; absorbs the
     * mp4 mdcv 0.0001-cd/m² normalisation drift. */
    return a.num * b.den == b.num * a.den;
}

}  // namespace

TEST_CASE("HDR re-encode propagates BT.2020 / PQ + ST 2086 + CTA-861.3 metadata") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_HDR;
    ME_REQUIRE_FIXTURE(fixture_path);

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    /* Snapshot input HDR metadata via probe. */
    InfoHandle in_info;
    const std::string in_uri = "file://" + fixture_path;
    REQUIRE(me_probe(eng.p, in_uri.c_str(), &in_info.p) == ME_OK);
    const me_hdr_static_metadata_t in_hdr =
        me_media_info_video_hdr_metadata(in_info.p);
    REQUIRE(in_hdr.has_mastering_display == 1);
    REQUIRE(in_hdr.has_content_light     == 1);

    /* Build a single-clip timeline against the HDR fixture; output
     * targets HEVC re-encode through hevc_videotoolbox. */
    char json[2048];
    std::snprintf(json, sizeof json,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"frameRate\":  {\"num\":24,\"den\":1},\n"
        "  \"resolution\": {\"width\":320,\"height\":240},\n"
        "  \"colorSpace\": {\"primaries\":\"bt2020\",\"transfer\":\"smpte2084\","
                          "\"matrix\":\"bt2020nc\",\"range\":\"limited\"},\n"
        "  \"assets\": [{\"id\":\"a1\",\"uri\":\"%s\"}],\n"
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
        "}\n", in_uri.c_str());

    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, json, std::strlen(json), &tl.p) == ME_OK);

    const std::string out_path =
        (fs::temp_directory_path() / "test_hdr_metadata_propagate_out.mp4").string();
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path           = out_path.c_str();
    spec.container      = "mp4";
    spec.video_codec    = "hevc";          /* hevc_videotoolbox per cycle 11 */
    spec.audio_codec    = "passthrough";
    spec.frame_rate.num = 24;
    spec.frame_rate.den = 1;

    RenderJobHandle job;
    me_status_t s = me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job.p);
    if (s == ME_E_UNSUPPORTED) {
        /* Host lacks hevc_videotoolbox. Same skip path as
         * test_encode_hevc_main10. */
        WARN("hevc encoder unavailable; skipping HDR re-encode round-trip");
        return;
    }
    REQUIRE(s == ME_OK);
    REQUIRE(me_render_wait(job.p) == ME_OK);

    /* Re-probe output and compare every HDR field. */
    InfoHandle out_info;
    const std::string out_uri = "file://" + out_path;
    REQUIRE(me_probe(eng.p, out_uri.c_str(), &out_info.p) == ME_OK);

    /* Codec / pixel / color tags: HEVC Main10 + bt2020 + smpte2084 +
     * bt2020nc + limited. */
    CHECK(std::string{me_media_info_video_codec(out_info.p)} == "hevc");
    CHECK(me_media_info_video_bit_depth(out_info.p) == 10);
    CHECK(std::string{me_media_info_video_color_primaries(out_info.p)} == "bt2020");
    CHECK(std::string{me_media_info_video_color_transfer(out_info.p)}  == "smpte2084");
    CHECK(std::string{me_media_info_video_color_space(out_info.p)}     == "bt2020nc");

    /* HDR side-data round-trip — the cycle's actual deliverable. */
    const me_hdr_static_metadata_t out_hdr =
        me_media_info_video_hdr_metadata(out_info.p);
    REQUIRE(out_hdr.has_mastering_display == 1);
    REQUIRE(out_hdr.has_content_light     == 1);

    /* Chromaticities (8 rationals) + luminance (2 rationals) round-trip
     * exactly via cross-multiply (mp4 mdcv normalises to 0.00002 /
     * 0.0001 unit denominators; rat_eq absorbs that). */
    CHECK(rat_eq_rt(out_hdr.mdcv_red_x,   in_hdr.mdcv_red_x));
    CHECK(rat_eq_rt(out_hdr.mdcv_red_y,   in_hdr.mdcv_red_y));
    CHECK(rat_eq_rt(out_hdr.mdcv_green_x, in_hdr.mdcv_green_x));
    CHECK(rat_eq_rt(out_hdr.mdcv_green_y, in_hdr.mdcv_green_y));
    CHECK(rat_eq_rt(out_hdr.mdcv_blue_x,  in_hdr.mdcv_blue_x));
    CHECK(rat_eq_rt(out_hdr.mdcv_blue_y,  in_hdr.mdcv_blue_y));
    CHECK(rat_eq_rt(out_hdr.mdcv_white_x, in_hdr.mdcv_white_x));
    CHECK(rat_eq_rt(out_hdr.mdcv_white_y, in_hdr.mdcv_white_y));
    CHECK(rat_eq_rt(out_hdr.mdcv_min_luminance, in_hdr.mdcv_min_luminance));
    CHECK(rat_eq_rt(out_hdr.mdcv_max_luminance, in_hdr.mdcv_max_luminance));

    /* CTA-861.3: integer values, exact equality. */
    CHECK(out_hdr.max_cll  == in_hdr.max_cll);
    CHECK(out_hdr.max_fall == in_hdr.max_fall);

    /* Sanity: input fixture wrote MaxCLL=1000, MaxFALL=400; the
     * pin lets a regression where the output drops side data
     * AND the input fixture also stops setting them produce a
     * clearer failure (matches cycle 14's positive-probe test
     * shape). */
    CHECK(in_hdr.max_cll  == 1000);
    CHECK(in_hdr.max_fall == 400);
    CHECK(rat_eq(in_hdr.mdcv_max_luminance, 1000, 1));

    /* Cleanup output file (best-effort; OS handles temp dir). */
    fs::remove(out_path);
}
