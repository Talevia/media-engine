/*
 * test_probe — me_probe + me_media_info_* accessor regressions.
 *
 * Before this suite existed, probe was only exercised via the `04_probe`
 * example — coverage came from eyeballing stdout against a dev machine.
 * The me-probe-more-fields cycle added 6 new accessors (rotation, color
 * range / primaries / transfer / space, bit depth) and deleted
 * probe.cpp's `return ME_E_UNSUPPORTED` path; without a unit test, future
 * refactors could silently regress any of these.
 *
 * Fixture reuse: the determinism fixture (built by gen_fixture.cpp) is a
 * BITEXACT-encoded 640×480 @ 25fps MPEG-4 Part 2 MP4 with no audio and no
 * color metadata. That makes it a good known-zero baseline for the new
 * color/rotation accessors — every field has a deterministic expected
 * value. A second fixture with explicit color tagging is a future gap
 * (test_probe_color_tagged would pair with an ffmpeg-CLI-generated asset
 * or a libav-generated one via a dedicated helper).
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include <filesystem>
#include <string>
#include "fixture_skip.hpp"

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

#ifndef ME_TEST_FIXTURE_MP4_HDR
#define ME_TEST_FIXTURE_MP4_HDR ""
#endif

namespace {

/* RAII helper so the engine + info handles don't leak across CHECK
 * branches. doctest evaluates through failed CHECKs (unlike REQUIRE), so
 * ad-hoc destroy/destroy chains can get skipped on an unexpected
 * failure; scope-guards stay. */
struct EngineHandle {
    me_engine_t* p = nullptr;
    ~EngineHandle() { if (p) me_engine_destroy(p); }
};
struct InfoHandle {
    me_media_info_t* p = nullptr;
    ~InfoHandle() { if (p) me_media_info_destroy(p); }
};

}  // namespace

TEST_CASE("me_probe extracts container + codec + dimensions + frame rate from fixture") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    InfoHandle info;
    const std::string uri = "file://" + fixture_path;
    REQUIRE(me_probe(eng.p, uri.c_str(), &info.p) == ME_OK);
    REQUIRE(info.p != nullptr);

    /* mov/mp4 family: libavformat's iformat name is a comma list; probe
     * keeps the first token, which on the mp4 muxer's decoder side is "mov". */
    CHECK(std::string{me_media_info_container(info.p)} == "mov");
    CHECK(me_media_info_has_video(info.p) == 1);
    CHECK(me_media_info_has_audio(info.p) == 0);   /* fixture has no audio track */

    /* Fixture spec (gen_fixture.cpp kWidth/kHeight/kFrameRate): */
    CHECK(me_media_info_video_width(info.p)  == 640);
    CHECK(me_media_info_video_height(info.p) == 480);
    CHECK(std::string{me_media_info_video_codec(info.p)} == "mpeg4");

    const me_rational_t fr = me_media_info_video_frame_rate(info.p);
    /* 25/1 is the encoded framerate; libavformat sometimes rewrites to an
     * equivalent rational (e.g. 50/2) but the ratio must be 25. */
    REQUIRE(fr.den > 0);
    CHECK((fr.num * 1) == (25 * fr.den) / 1);  /* num/den == 25 */
}

TEST_CASE("me_probe extracts extended video metadata (rotation / color / bit_depth)") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    InfoHandle info;
    const std::string uri = "file://" + fixture_path;
    REQUIRE(me_probe(eng.p, uri.c_str(), &info.p) == ME_OK);

    /* gen_fixture doesn't stamp a display matrix or color tags, so every
     * new accessor sees its library-default "unknown". Bit depth comes
     * from the pixel-format descriptor (YUV420P has comp[0].depth == 8). */
    CHECK(me_media_info_video_rotation(info.p) == 0);
    CHECK(me_media_info_video_bit_depth(info.p) == 8);

    /* av_color_*_name returns "unknown" (not empty) for UNSPECIFIED enum
     * values — the accessor just forwards that string. */
    CHECK(std::string{me_media_info_video_color_range(info.p)}     == "unknown");
    CHECK(std::string{me_media_info_video_color_primaries(info.p)} == "unknown");
    CHECK(std::string{me_media_info_video_color_transfer(info.p)}  == "unknown");
    CHECK(std::string{me_media_info_video_color_space(info.p)}     == "unknown");
}

TEST_CASE("me_probe extracts HDR static metadata from HDR fixture (HDR10 / ST 2086)") {
    /* Positive-path counterpart to the SDR all-zero case below.
     * Fixture is `tests/fixtures/gen_hdr_fixture.cpp`'s HEVC Main10
     * mp4 with AVMasteringDisplayMetadata (BT.2020 primaries,
     * 0.0001 → 1000 nits) + AVContentLightMetadata (MaxCLL=1000,
     * MaxFALL=400) attached. Re-probing must surface every field
     * the fixture writes. Skipped on hosts where gen_hdr_fixture
     * couldn't link (e.g. non-VideoToolbox CI). */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_HDR;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    InfoHandle info;
    const std::string uri = "file://" + fixture_path;
    REQUIRE(me_probe(eng.p, uri.c_str(), &info.p) == ME_OK);

    /* Sanity: codec is HEVC, pix_fmt is 10-bit. */
    CHECK(std::string{me_media_info_video_codec(info.p)} == "hevc");
    CHECK(me_media_info_video_bit_depth(info.p) == 10);
    CHECK(std::string{me_media_info_video_color_primaries(info.p)} == "bt2020");
    CHECK(std::string{me_media_info_video_color_transfer(info.p)}  == "smpte2084");
    CHECK(std::string{me_media_info_video_color_space(info.p)}     == "bt2020nc");

    /* HDR metadata. Compare rationals as cross-multiplied scalars
     * — the mp4 mdcv box stores luminance in 0.0001 cd/m² units
     * and chromaticities in 0.00002 units, so libavformat's demux
     * surfaces "1000 cd/m²" as `{10000000, 10000}` not `{1000, 1}`.
     * Both denote the same value; the round-trip preserves the
     * value but not the rational normalization. */
    const me_hdr_static_metadata_t hdr = me_media_info_video_hdr_metadata(info.p);
    REQUIRE(hdr.has_mastering_display == 1);
    REQUIRE(hdr.has_content_light     == 1);

    auto rat_eq = [](me_rational_t a, int64_t num, int64_t den) {
        /* a == num/den ⇔ a.num * den == num * a.den. */
        return a.num * den == num * a.den;
    };

    /* BT.2020 chromaticities (0.708, 0.292) etc. — see
     * gen_hdr_fixture.cpp's `kRedX` / `kRedY` constants. */
    CHECK(rat_eq(hdr.mdcv_red_x,   35400, 50000));
    CHECK(rat_eq(hdr.mdcv_red_y,   14600, 50000));
    CHECK(rat_eq(hdr.mdcv_green_x,  8500, 50000));
    CHECK(rat_eq(hdr.mdcv_green_y, 39850, 50000));
    CHECK(rat_eq(hdr.mdcv_blue_x,   6550, 50000));
    CHECK(rat_eq(hdr.mdcv_blue_y,   2300, 50000));
    CHECK(rat_eq(hdr.mdcv_white_x, 15635, 50000));
    CHECK(rat_eq(hdr.mdcv_white_y, 16450, 50000));
    /* Min luminance 0.0001 cd/m² (1/10000), max 1000 cd/m². */
    CHECK(rat_eq(hdr.mdcv_min_luminance, 1,    10000));
    CHECK(rat_eq(hdr.mdcv_max_luminance, 1000, 1));

    /* CTA-861.3 content light values — uint, not rational. */
    CHECK(hdr.max_cll  == 1000);
    CHECK(hdr.max_fall == 400);
}

TEST_CASE("me_probe reports no HDR metadata for SDR fixture") {
    /* Determinism fixture is plain SDR mpeg4 with no MASTERING_DISPLAY /
     * CONTENT_LIGHT side data attached. Every HDR field must be its
     * default-zero value — proves the extractor doesn't false-positive
     * on streams that lack the side data, and that the by-value
     * struct returns cleanly across the C ABI. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    InfoHandle info;
    const std::string uri = "file://" + fixture_path;
    REQUIRE(me_probe(eng.p, uri.c_str(), &info.p) == ME_OK);

    const me_hdr_static_metadata_t hdr = me_media_info_video_hdr_metadata(info.p);
    CHECK(hdr.has_mastering_display == 0);
    CHECK(hdr.has_content_light     == 0);
    CHECK(hdr.max_cll               == 0);
    CHECK(hdr.max_fall              == 0);
    /* Chromaticity / luminance fields stay at their default {0, 1}
     * — `den == 1` keeps them well-formed rationals at the C ABI. */
    CHECK(hdr.mdcv_red_x.num   == 0);
    CHECK(hdr.mdcv_red_x.den   == 1);
    CHECK(hdr.mdcv_max_luminance.num == 0);
    CHECK(hdr.mdcv_max_luminance.den == 1);
}

TEST_CASE("me_probe returns ME_E_IO for a non-existent URI") {
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    me_media_info_t* info = nullptr;
    const me_status_t s = me_probe(eng.p, "file:///nonexistent/path/not-a-file.mp4", &info);
    CHECK(s == ME_E_IO);
    CHECK(info == nullptr);   /* probe clears *out on failure (API.md contract) */

    /* Non-empty last_error is a usability requirement — the status alone
     * doesn't tell the host *why* the open failed. */
    const char* le = me_engine_last_error(eng.p);
    REQUIRE(le != nullptr);
    CHECK(std::string{le}.find("avformat_open_input") != std::string::npos);
}

TEST_CASE("me_probe rejects null arguments with ME_E_INVALID_ARG") {
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    me_media_info_t* info = nullptr;
    CHECK(me_probe(nullptr,  "file:///x.mp4", &info) == ME_E_INVALID_ARG);
    CHECK(me_probe(eng.p,    nullptr,         &info) == ME_E_INVALID_ARG);
    CHECK(me_probe(eng.p,    "file:///x.mp4", nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("me_media_info_* accessors tolerate null info pointer") {
    /* Defensive: callers occasionally pass through a null me_media_info_t
     * (failed probe followed by stats dump, etc.). Every accessor must
     * degrade to a documented default rather than dereferencing null. */
    CHECK(std::string{me_media_info_container(nullptr)} == "");
    CHECK(me_media_info_has_video(nullptr) == 0);
    CHECK(me_media_info_has_audio(nullptr) == 0);
    CHECK(me_media_info_video_width(nullptr) == 0);
    CHECK(me_media_info_video_height(nullptr) == 0);
    CHECK(me_media_info_video_rotation(nullptr) == 0);
    CHECK(me_media_info_video_bit_depth(nullptr) == 0);
    CHECK(me_media_info_audio_sample_rate(nullptr) == 0);
    CHECK(me_media_info_audio_channels(nullptr) == 0);

    const me_rational_t dur = me_media_info_duration(nullptr);
    CHECK(dur.num == 0);

    const me_rational_t fr = me_media_info_video_frame_rate(nullptr);
    CHECK(fr.num == 0);

    CHECK(std::string{me_media_info_video_codec(nullptr)} == "");
    CHECK(std::string{me_media_info_audio_codec(nullptr)} == "");
    CHECK(std::string{me_media_info_video_color_range(nullptr)} == "");
    CHECK(std::string{me_media_info_video_color_primaries(nullptr)} == "");
    CHECK(std::string{me_media_info_video_color_transfer(nullptr)} == "");
    CHECK(std::string{me_media_info_video_color_space(nullptr)} == "");

    /* HDR metadata accessor returns an all-zero struct on null. */
    const me_hdr_static_metadata_t hdr = me_media_info_video_hdr_metadata(nullptr);
    CHECK(hdr.has_mastering_display == 0);
    CHECK(hdr.has_content_light     == 0);
    CHECK(hdr.max_cll               == 0);
    CHECK(hdr.max_fall              == 0);
}
