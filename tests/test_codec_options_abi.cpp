/*
 * test_codec_options_abi — pin the cycle-47 me_output_spec_t
 * append-only ABI evolution (M7-debt me-output-spec-typed-codec-enum).
 *
 * Verifies:
 *   1. Zero-init me_output_spec_t leaves the new `codec_options`
 *      field as NULL (legacy hosts unaffected; the precedence rule
 *      "NULL = use string fields" holds).
 *   2. me_codec_options_t accepts the typed enum + per-codec opts
 *      pointers — primary new-host construction shape.
 *   3. Enum values are stable: NONE = 0, then PASSTHROUGH / H264 /
 *      HEVC / HEVC_SW for video and NONE / PASSTHROUGH / AAC for
 *      audio. These are ABI-pinned per §3a.10; never re-numbered.
 *   4. Per-codec opts structs are POD (trivially constructible +
 *     `bitrate_bps == 0` zero-init means "engine default" the
 *     same way the legacy `video_bitrate_bps` does).
 *
 * Pixel correctness / sink integration tests stay in
 * test_output_sink.cpp etc.; this test only pins the public-ABI
 * shape so a future struct-mutation regression surfaces here
 * before it reaches a host.
 */
#include <doctest/doctest.h>

#include "media_engine/codec_options.h"
#include "media_engine/render.h"

#include <type_traits>

TEST_CASE("me_output_spec_t zero-init leaves codec_options NULL") {
    me_output_spec_t spec{};
    CHECK(spec.codec_options == nullptr);
    /* Legacy string fields also NULL by zero-init — the existing
     * "set what you need, leave the rest" idiom continues to work. */
    CHECK(spec.video_codec == nullptr);
    CHECK(spec.audio_codec == nullptr);
}

TEST_CASE("me_codec_options_t accepts typed enum + per-codec opts") {
    me_h264_opts_t   h264{};
    h264.bitrate_bps = 5'000'000;
    me_aac_opts_t    aac{};
    aac.bitrate_bps  = 128'000;

    me_codec_options_t opts{};
    opts.video_codec = ME_VIDEO_CODEC_H264;
    opts.audio_codec = ME_AUDIO_CODEC_AAC;
    opts.h264 = &h264;
    opts.aac  = &aac;

    me_output_spec_t spec{};
    spec.path           = "/tmp/x.mp4";
    spec.container      = "mp4";
    spec.codec_options  = &opts;

    REQUIRE(spec.codec_options == &opts);
    CHECK(spec.codec_options->video_codec == ME_VIDEO_CODEC_H264);
    CHECK(spec.codec_options->audio_codec == ME_AUDIO_CODEC_AAC);
    REQUIRE(spec.codec_options->h264 != nullptr);
    REQUIRE(spec.codec_options->aac  != nullptr);
    CHECK(spec.codec_options->h264->bitrate_bps == 5'000'000);
    CHECK(spec.codec_options->aac ->bitrate_bps == 128'000);
    /* Codec slots that aren't selected stay NULL. */
    CHECK(spec.codec_options->hevc    == nullptr);
    CHECK(spec.codec_options->hevc_sw == nullptr);
}

TEST_CASE("me_video_codec_t enum values are ABI-pinned") {
    /* These are part of the public ABI per §3a.10; never re-number.
     * If a future commit changes any of these, this test must fail
     * loudly so a host with a hardcoded literal doesn't silently
     * pick up a different codec. */
    CHECK(static_cast<int>(ME_VIDEO_CODEC_NONE)        == 0);
    CHECK(static_cast<int>(ME_VIDEO_CODEC_PASSTHROUGH) == 1);
    CHECK(static_cast<int>(ME_VIDEO_CODEC_H264)        == 2);
    CHECK(static_cast<int>(ME_VIDEO_CODEC_HEVC)        == 3);
    CHECK(static_cast<int>(ME_VIDEO_CODEC_HEVC_SW)     == 4);
}

TEST_CASE("me_audio_codec_t enum values are ABI-pinned") {
    CHECK(static_cast<int>(ME_AUDIO_CODEC_NONE)        == 0);
    CHECK(static_cast<int>(ME_AUDIO_CODEC_PASSTHROUGH) == 1);
    CHECK(static_cast<int>(ME_AUDIO_CODEC_AAC)         == 2);
}

TEST_CASE("per-codec opts structs are POD with zero-init defaults") {
    /* Trivial constructibility = host can stack-allocate without
     * worrying about C++ side effects. */
    static_assert(std::is_trivial_v<me_h264_opts_t>);
    static_assert(std::is_trivial_v<me_hevc_opts_t>);
    static_assert(std::is_trivial_v<me_hevc_sw_opts_t>);
    static_assert(std::is_trivial_v<me_aac_opts_t>);
    static_assert(std::is_trivial_v<me_codec_options_t>);

    /* Zero-init = "engine defaults for this codec" — bitrate 0
     * matches the legacy "video_bitrate_bps == 0 means default"
     * semantic from me_output_spec_t. */
    me_h264_opts_t h264{};
    me_hevc_opts_t hevc{};
    me_hevc_sw_opts_t hevc_sw{};
    me_aac_opts_t aac{};
    CHECK(h264.bitrate_bps    == 0);
    CHECK(hevc.bitrate_bps    == 0);
    CHECK(hevc_sw.bitrate_bps == 0);
    CHECK(aac.bitrate_bps     == 0);
}

TEST_CASE("me_codec_options_t default selection means NONE (host opts in explicitly)") {
    me_codec_options_t opts{};
    /* Zero-init aggregator selects NONE for both — the resolver's
     * documented "NONE means fall back to string fields"
     * behavior. A host that constructs a default me_codec_options_t
     * AND attaches it to me_output_spec_t.codec_options gets the
     * legacy string-dispatch path back unchanged. */
    CHECK(opts.video_codec == ME_VIDEO_CODEC_NONE);
    CHECK(opts.audio_codec == ME_AUDIO_CODEC_NONE);
    CHECK(opts.h264    == nullptr);
    CHECK(opts.hevc    == nullptr);
    CHECK(opts.hevc_sw == nullptr);
    CHECK(opts.aac     == nullptr);
}
