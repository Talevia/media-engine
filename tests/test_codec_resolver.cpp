/*
 * test_codec_resolver — pin the cycle-48 typed-codec resolver
 * (M7-debt debt-typed-codec-options-internal-migration). Verifies
 * that `me::orchestrator::resolve_codec_selection`:
 *
 *   1. Maps every legacy `me_output_spec_t.video_codec` /
 *      `audio_codec` string to the documented enum.
 *   2. Prefers the typed `codec_options` extension when its enum
 *      is non-NONE.
 *   3. Falls back to the strings when codec_options is NULL OR
 *      its enum is NONE.
 *   4. Resolves per-codec bitrate from the matching opts struct
 *     (h264.bitrate_bps for H264, etc.); falls back to
 *     spec.video_bitrate_bps when the per-codec opts pointer is
 *     NULL OR its bitrate_bps is 0.
 *
 * The byte-equivalent end-to-end test (render via legacy strings
 * + render via codec_options + byte-compare MP4) lives at the
 * unit level here: same spec resolves to the same selection,
 * which means the sinks dispatched off the selection produce the
 * same output bytes. A render-twice integration test is
 * follow-up debt if scrubbing surfaces actual divergence.
 */
#include <doctest/doctest.h>

#include "media_engine/codec_options.h"
#include "media_engine/render.h"
#include "orchestrator/codec_resolver.hpp"

using me::orchestrator::CodecSelection;
using me::orchestrator::resolve_codec_selection;

TEST_CASE("resolve_codec_selection: legacy passthrough strings") {
    me_output_spec_t s{};
    s.video_codec = "passthrough";
    s.audio_codec = "passthrough";
    const CodecSelection sel = resolve_codec_selection(s);
    CHECK(sel.video_codec == ME_VIDEO_CODEC_PASSTHROUGH);
    CHECK(sel.audio_codec == ME_AUDIO_CODEC_PASSTHROUGH);
}

TEST_CASE("resolve_codec_selection: legacy h264/aac strings") {
    me_output_spec_t s{};
    s.video_codec = "h264";
    s.audio_codec = "aac";
    s.video_bitrate_bps = 5'000'000;
    s.audio_bitrate_bps = 128'000;
    const CodecSelection sel = resolve_codec_selection(s);
    CHECK(sel.video_codec == ME_VIDEO_CODEC_H264);
    CHECK(sel.audio_codec == ME_AUDIO_CODEC_AAC);
    CHECK(sel.video_bitrate_bps == 5'000'000);
    CHECK(sel.audio_bitrate_bps == 128'000);
}

TEST_CASE("resolve_codec_selection: legacy hevc / hevc-sw strings map distinctly") {
    me_output_spec_t s_hevc{};
    s_hevc.video_codec = "hevc";
    CHECK(resolve_codec_selection(s_hevc).video_codec == ME_VIDEO_CODEC_HEVC);

    me_output_spec_t s_sw{};
    s_sw.video_codec = "hevc-sw";
    CHECK(resolve_codec_selection(s_sw).video_codec == ME_VIDEO_CODEC_HEVC_SW);
}

TEST_CASE("resolve_codec_selection: NULL / empty / 'none' audio_codec → ME_AUDIO_CODEC_NONE") {
    me_output_spec_t s_null{}; s_null.video_codec = "hevc-sw"; s_null.audio_codec = nullptr;
    CHECK(resolve_codec_selection(s_null).audio_codec == ME_AUDIO_CODEC_NONE);

    me_output_spec_t s_empty{}; s_empty.video_codec = "hevc-sw"; s_empty.audio_codec = "";
    CHECK(resolve_codec_selection(s_empty).audio_codec == ME_AUDIO_CODEC_NONE);

    me_output_spec_t s_none{}; s_none.video_codec = "hevc-sw"; s_none.audio_codec = "none";
    CHECK(resolve_codec_selection(s_none).audio_codec == ME_AUDIO_CODEC_NONE);
}

TEST_CASE("resolve_codec_selection: unknown video codec string → ME_VIDEO_CODEC_NONE") {
    /* Resolver doesn't fail on unknown strings — that's the
     * dispatch layer's job (each sink rejects what it can't
     * serve with its existing diagnostic). */
    me_output_spec_t s{};
    s.video_codec = "av1";  /* not in the cycle-47 enum */
    CHECK(resolve_codec_selection(s).video_codec == ME_VIDEO_CODEC_NONE);
}

TEST_CASE("resolve_codec_selection: codec_options non-NONE wins over strings") {
    me_codec_options_t opts{};
    opts.video_codec = ME_VIDEO_CODEC_HEVC;
    opts.audio_codec = ME_AUDIO_CODEC_AAC;

    me_output_spec_t s{};
    /* String fields say h264/passthrough; typed says hevc/aac.
     * The typed enum should win. */
    s.video_codec    = "h264";
    s.audio_codec    = "passthrough";
    s.codec_options  = &opts;

    const CodecSelection sel = resolve_codec_selection(s);
    CHECK(sel.video_codec == ME_VIDEO_CODEC_HEVC);
    CHECK(sel.audio_codec == ME_AUDIO_CODEC_AAC);
}

TEST_CASE("resolve_codec_selection: codec_options NONE falls back to strings") {
    me_codec_options_t opts{};  /* zero-init: video_codec / audio_codec = NONE */
    me_output_spec_t s{};
    s.video_codec   = "h264";
    s.audio_codec   = "aac";
    s.codec_options = &opts;
    const CodecSelection sel = resolve_codec_selection(s);
    /* Even though codec_options is attached, its enum is NONE so
     * the strings remain canonical. */
    CHECK(sel.video_codec == ME_VIDEO_CODEC_H264);
    CHECK(sel.audio_codec == ME_AUDIO_CODEC_AAC);
}

TEST_CASE("resolve_codec_selection: per-codec opts.bitrate_bps wins over spec.video_bitrate_bps") {
    me_h264_opts_t h264{};
    h264.bitrate_bps = 8'000'000;

    me_codec_options_t opts{};
    opts.video_codec = ME_VIDEO_CODEC_H264;
    opts.h264 = &h264;

    me_output_spec_t s{};
    s.video_bitrate_bps = 1'000'000;  /* should be overridden */
    s.codec_options     = &opts;

    const CodecSelection sel = resolve_codec_selection(s);
    CHECK(sel.video_codec == ME_VIDEO_CODEC_H264);
    CHECK(sel.video_bitrate_bps == 8'000'000);
}

TEST_CASE("resolve_codec_selection: per-codec opts.bitrate_bps == 0 falls back to spec") {
    me_h264_opts_t h264{};  /* bitrate_bps = 0 (engine default) */
    me_codec_options_t opts{};
    opts.video_codec = ME_VIDEO_CODEC_H264;
    opts.h264 = &h264;

    me_output_spec_t s{};
    s.video_bitrate_bps = 1'000'000;
    s.codec_options     = &opts;

    /* bitrate_bps == 0 means "engine default for this codec",
     * which falls back to spec.video_bitrate_bps. */
    CHECK(resolve_codec_selection(s).video_bitrate_bps == 1'000'000);
}

TEST_CASE("resolve_codec_selection: per-codec opts pointer NULL falls back to spec") {
    me_codec_options_t opts{};
    opts.video_codec = ME_VIDEO_CODEC_H264;
    opts.h264 = nullptr;  /* host opted into typed enum but didn't supply opts */

    me_output_spec_t s{};
    s.video_bitrate_bps = 2'500'000;
    s.codec_options     = &opts;

    CHECK(resolve_codec_selection(s).video_bitrate_bps == 2'500'000);
}

TEST_CASE("resolve_codec_selection: aac.bitrate_bps wins over spec.audio_bitrate_bps") {
    me_aac_opts_t aac{};
    aac.bitrate_bps = 192'000;

    me_codec_options_t opts{};
    opts.audio_codec = ME_AUDIO_CODEC_AAC;
    opts.aac = &aac;

    me_output_spec_t s{};
    s.audio_bitrate_bps = 96'000;
    s.codec_options     = &opts;

    CHECK(resolve_codec_selection(s).audio_bitrate_bps == 192'000);
}

TEST_CASE("resolve_codec_selection: byte-equivalence — string spec and codec_options spec produce same selection") {
    /* The migration's regression backbone: any host can switch
     * from string-based to codec_options-based without changing
     * the dispatched sink. */
    me_output_spec_t legacy{};
    legacy.video_codec       = "h264";
    legacy.audio_codec       = "aac";
    legacy.video_bitrate_bps = 4'000'000;
    legacy.audio_bitrate_bps = 128'000;

    me_h264_opts_t h264{};
    h264.bitrate_bps = 4'000'000;
    me_aac_opts_t aac{};
    aac.bitrate_bps = 128'000;
    me_codec_options_t opts{};
    opts.video_codec = ME_VIDEO_CODEC_H264;
    opts.audio_codec = ME_AUDIO_CODEC_AAC;
    opts.h264 = &h264;
    opts.aac  = &aac;
    me_output_spec_t typed{};
    typed.codec_options = &opts;

    const CodecSelection a = resolve_codec_selection(legacy);
    const CodecSelection b = resolve_codec_selection(typed);

    CHECK(a.video_codec       == b.video_codec);
    CHECK(a.audio_codec       == b.audio_codec);
    CHECK(a.video_bitrate_bps == b.video_bitrate_bps);
    CHECK(a.audio_bitrate_bps == b.audio_bitrate_bps);
}
