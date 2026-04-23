/*
 * test_output_sink — direct coverage of make_output_sink's classification
 * and rejection paths.
 *
 * Before this suite existed, PassthroughSink / H264AacSink were only
 * exercised *indirectly* via 01_passthrough / 05_reencode — an end-to-end
 * coverage shape where an intended rejection (unsupported spec, empty
 * clip ranges, h264 + multi-clip) and an unintended regression (wrong
 * error string, null deref, factory returning a stub) would both show up
 * as the same "render failed" signal. These tests assert factory return
 * values and the diagnostic substring in `err`, so a future refactor of
 * the rejection wording is a deliberate decision rather than a silent
 * contract drift.
 *
 * process() is intentionally *not* called here — the integration examples
 * cover the happy path; the factory's job is to classify and gate, and
 * that's what we exercise.
 */
#include <doctest/doctest.h>

#include <media_engine/render.h>
#include <media_engine/types.h>

#include "orchestrator/output_sink.hpp"
#include "resource/codec_pool.hpp"

#include <string>
#include <vector>

using me::orchestrator::ClipTimeRange;
using me::orchestrator::make_output_sink;
using me::orchestrator::SinkCommon;

namespace {

/* Build a single-clip range list with deterministic rationals. */
std::vector<ClipTimeRange> one_clip() {
    return std::vector<ClipTimeRange>{
        ClipTimeRange{
            .source_start    = me_rational_t{0, 1},
            .source_duration = me_rational_t{1, 1},
            .time_offset     = me_rational_t{0, 1},
        },
    };
}

std::vector<ClipTimeRange> two_clips() {
    return std::vector<ClipTimeRange>{
        ClipTimeRange{{0, 1}, {1, 1}, {0, 1}},
        ClipTimeRange{{0, 1}, {1, 1}, {1, 1}},
    };
}

me_output_spec_t passthrough_spec() {
    me_output_spec_t s{};
    s.path         = "/tmp/out.mp4";
    s.container    = "mp4";
    s.video_codec  = "passthrough";
    s.audio_codec  = "passthrough";
    return s;
}

me_output_spec_t h264_aac_spec() {
    me_output_spec_t s{};
    s.path              = "/tmp/out.mp4";
    s.container         = "mp4";
    s.video_codec       = "h264";
    s.audio_codec       = "aac";
    s.video_bitrate_bps = 2'000'000;
    s.audio_bitrate_bps = 128'000;
    return s;
}

SinkCommon blank_common() {
    SinkCommon c;
    c.out_path  = "/tmp/out.mp4";
    c.container = "mp4";
    return c;
}

}  // namespace

TEST_CASE("make_output_sink builds a PassthroughSink for passthrough spec") {
    me::resource::CodecPool pool;
    std::string err;
    auto sink = make_output_sink(passthrough_spec(), blank_common(), one_clip(),
                                  &pool, &err);
    CHECK(sink != nullptr);
    CHECK(err.empty());
}

TEST_CASE("make_output_sink builds an H264AacSink for single-clip h264/aac spec") {
    me::resource::CodecPool pool;
    std::string err;
    auto sink = make_output_sink(h264_aac_spec(), blank_common(), one_clip(),
                                  &pool, &err);
    CHECK(sink != nullptr);
    CHECK(err.empty());
}

TEST_CASE("make_output_sink rejects null spec.path") {
    me::resource::CodecPool pool;
    me_output_spec_t spec = passthrough_spec();
    spec.path = nullptr;

    std::string err;
    auto sink = make_output_sink(spec, blank_common(), one_clip(), &pool, &err);
    CHECK(sink == nullptr);
    CHECK(err.find("output.path") != std::string::npos);
}

TEST_CASE("make_output_sink rejects empty clip_ranges") {
    me::resource::CodecPool pool;
    std::string err;
    auto sink = make_output_sink(passthrough_spec(), blank_common(),
                                  std::vector<ClipTimeRange>{}, &pool, &err);
    CHECK(sink == nullptr);
    CHECK(err.find("at least one clip") != std::string::npos);
}

TEST_CASE("make_output_sink rejects mixed h264 + passthrough spec") {
    me::resource::CodecPool pool;
    me_output_spec_t spec = h264_aac_spec();
    spec.audio_codec = "passthrough";   /* h264 video + passthrough audio: unsupported */

    std::string err;
    auto sink = make_output_sink(spec, blank_common(), one_clip(), &pool, &err);
    CHECK(sink == nullptr);
    CHECK(err.find("supported specs") != std::string::npos);
}

TEST_CASE("make_output_sink rejects h264/aac + multi-clip spec") {
    me::resource::CodecPool pool;
    std::string err;
    auto sink = make_output_sink(h264_aac_spec(), blank_common(), two_clips(),
                                  &pool, &err);
    CHECK(sink == nullptr);
    CHECK(err.find("single clip") != std::string::npos);
}
