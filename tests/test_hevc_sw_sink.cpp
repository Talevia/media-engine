/*
 * test_hevc_sw_sink — covers `me::orchestrator::HevcSwSink`, the
 * SW HEVC fallback that completes the dispatch wired by the prior
 * cycle (`encode-hevc-output-sink-runtime-sw-dispatch`).
 *
 * Spec-match unit tests (always run):
 *   - The inlined (HEVC_SW, NONE) shape match in make_output_sink
 *     accepts (hevc-sw, none/empty/null) via resolve_codec_selection.
 *   - rejects (hevc-sw, aac) — that combo is owned by VideoAacSink
 *     which UNSUPPORTEDs at preflight.
 *   - rejects (h264, *) and (passthrough, *).
 *
 * Integration test (gated on ME_HAS_KVAZAAR):
 *   - Drive the determinism fixture (640x480 YUV420P MPEG-4 Part 2,
 *     25 fps, 25 frames) through HevcSwSink → assert the output
 *     `.hevc` file is non-empty + leads with an Annex-B start code
 *     + a (parsed-from-NAL) HEVC parameter-set NAL, i.e. a valid
 *     raw HEVC bitstream.
 *
 * Skips when ME_HAS_KVAZAAR isn't defined (Kvazaar not linked) —
 * mirrors test_kvazaar_hevc_encoder's gate so OFF builds still
 * compile but emit one "skipped" TEST_CASE.
 */
#include <doctest/doctest.h>

#include "media_engine/types.h"
#include "orchestrator/codec_resolver.hpp"
#include "orchestrator/hevc_sw_sink.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#ifdef ME_HAS_KVAZAAR
#include "io/demux_context.hpp"
extern "C" {
#include <libavformat/avformat.h>
}
#endif

namespace {

me_output_spec_t make_spec(const char* video, const char* audio) {
    me_output_spec_t s{};
    s.video_codec = video;
    s.audio_codec = audio;
    return s;
}

}  // namespace

namespace {

/* Re-implements the inlined (HEVC_SW, NONE) shape match from
 * make_output_sink (src/orchestrator/output_sink.cpp). The check
 * is two enum comparisons against the resolver result; this
 * helper exists so the test cases below read like the prior
 * `is_hevc_sw_video_only_spec` tests but exercise the actual
 * dispatch shape. */
bool is_hevc_sw_video_only_selection(const me_output_spec_t& spec) {
    const auto sel = me::orchestrator::resolve_codec_selection(spec);
    return sel.video_codec == ME_VIDEO_CODEC_HEVC_SW
        && sel.audio_codec == ME_AUDIO_CODEC_NONE;
}

}  // namespace

TEST_CASE("hevc-sw dispatch: matches (hevc-sw, none)") {
    auto s = make_spec("hevc-sw", "none");
    CHECK(is_hevc_sw_video_only_selection(s));
}

TEST_CASE("hevc-sw dispatch: matches (hevc-sw, NULL)") {
    auto s = make_spec("hevc-sw", nullptr);
    CHECK(is_hevc_sw_video_only_selection(s));
}

TEST_CASE("hevc-sw dispatch: matches (hevc-sw, empty)") {
    auto s = make_spec("hevc-sw", "");
    CHECK(is_hevc_sw_video_only_selection(s));
}

TEST_CASE("hevc-sw dispatch: rejects (hevc-sw, aac)") {
    /* (hevc-sw, aac) is the VideoAacSink shape — that path goes to
     * `open_video_encoder`'s preflight which returns ME_E_UNSUPPORTED.
     * The HevcSwSink only owns the no-audio combo. */
    auto s = make_spec("hevc-sw", "aac");
    CHECK_FALSE(is_hevc_sw_video_only_selection(s));
}

TEST_CASE("hevc-sw dispatch: rejects (h264, none)") {
    auto s = make_spec("h264", "none");
    CHECK_FALSE(is_hevc_sw_video_only_selection(s));
}

TEST_CASE("hevc-sw dispatch: rejects (NULL, NULL)") {
    auto s = make_spec(nullptr, nullptr);
    CHECK_FALSE(is_hevc_sw_video_only_selection(s));
}

TEST_CASE("hevc-sw dispatch: rejects (passthrough, passthrough)") {
    auto s = make_spec("passthrough", "passthrough");
    CHECK_FALSE(is_hevc_sw_video_only_selection(s));
}

#ifdef ME_HAS_KVAZAAR

TEST_CASE("HevcSwSink: encodes 640x480 fixture into raw Annex-B HEVC") {
    /* The fixture path is wired in by tests/CMakeLists.txt as a
     * compile-def `ME_TEST_FIXTURE_MP4`. */
#ifndef ME_TEST_FIXTURE_MP4
    /* Nothing to test — the fixture wiring is missing. */
    REQUIRE_MESSAGE(false, "ME_TEST_FIXTURE_MP4 not defined; check tests/CMakeLists.txt");
#else
    /* Stage 1: open the fixture as a demux context. */
    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, ME_TEST_FIXTURE_MP4, nullptr, nullptr);
    REQUIRE(rc == 0);
    rc = avformat_find_stream_info(fmt, nullptr);
    REQUIRE(rc >= 0);

    auto demux = std::make_shared<me::io::DemuxContext>();
    demux->fmt = fmt;
    /* DemuxContext owns fmt and frees it on destruction. */

    /* Stage 2: build the sink. */
    me::orchestrator::SinkCommon common;
    common.out_path = std::string(ME_TEST_FIXTURE_MP4) + ".hevc";
    common.container = "hevc";
    /* Remove any prior output so file-existence is meaningful. */
    std::remove(common.out_path.c_str());

    std::vector<me::orchestrator::ClipTimeRange> ranges;
    ranges.push_back({});  /* default range = whole input */
    auto sink = me::orchestrator::make_hevc_sw_sink(std::move(common),
                                                     std::move(ranges));
    REQUIRE(sink != nullptr);

    /* Stage 3: process. */
    std::string err;
    std::vector<std::shared_ptr<me::io::DemuxContext>> demuxes{demux};
    const me_status_t s = sink->process(std::move(demuxes), &err);
    REQUIRE_MESSAGE(s == ME_OK, err);

    /* Stage 4: open the output and verify it's a non-empty Annex-B
     * HEVC bitstream. The first 4 bytes must be a start code
     * (0x00 0x00 0x00 0x01); the 5th byte's NAL type (high 6 bits of
     * the first byte after start code, shifted right 1, then masked
     * 0x3F) should be in [32, 33, 34] = VPS/SPS/PPS NAL unit types
     * for HEVC (Annex B leads streams with parameter sets). */
    const std::string out_path = std::string(ME_TEST_FIXTURE_MP4) + ".hevc";
    std::ifstream f(out_path, std::ios::binary);
    REQUIRE(f.good());
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 0);
    /* HEVC NALs in Annex-B start with 0x00 0x00 0x00 0x01 OR
     * 0x00 0x00 0x01. Either is acceptable. */
    auto starts_with_startcode = [&]() -> bool {
        if (bytes.size() < 3) return false;
        if (bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x01) return true;
        if (bytes.size() >= 4 && bytes[0] == 0x00 && bytes[1] == 0x00 &&
            bytes[2] == 0x00 && bytes[3] == 0x01) return true;
        return false;
    };
    CHECK(starts_with_startcode());

    /* HEVC NAL unit type lives in bits [1..6] of the first byte
     * after the start code (forbidden_zero_bit + nal_unit_type). */
    std::size_t nal_start = (bytes[2] == 0x01) ? 3 : 4;
    REQUIRE(bytes.size() > nal_start);
    const std::uint8_t nal_type = (bytes[nal_start] >> 1) & 0x3F;
    /* HEVC NAL types: VPS=32, SPS=33, PPS=34, AUD=35, IDR_W_RADL=19,
     * IDR_N_LP=20. Kvazaar emits VPS/SPS/PPS as the first NALs of
     * the stream; if the encoder later emits an AUD ahead, allow
     * that too. */
    CHECK((nal_type == 32 || nal_type == 33 || nal_type == 34 || nal_type == 35));

    /* Cleanup the synthesized output so re-runs don't leave detritus. */
    std::remove(out_path.c_str());
#endif
}

#else  /* !ME_HAS_KVAZAAR */

TEST_CASE("HevcSwSink: skipped (ME_HAS_KVAZAAR not defined at test build)") {
    /* Build-flag-gated stub mirror of test_kvazaar_hevc_encoder's
     * skip case. The factory still returns a non-null sink in this
     * mode (the stubs route to HevcSwSinkUnsupported which UNSUPPORTEDs
     * at process() time), but exercising it requires libavformat
     * which the OFF-build test env may not link. */
}

#endif
