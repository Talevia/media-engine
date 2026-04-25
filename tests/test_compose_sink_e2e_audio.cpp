/*
 * test_compose_sink_e2e_audio — extracted from the original
 * 1136-line test_compose_sink_e2e.cpp when the file was split
 * by pipeline kind (debt-split-test-compose-sink-e2e-cpp).
 * Shared handle RAIIs + fixtures live in
 * compose_sink_e2e_fixtures.hpp.
 */
#include "compose_sink_e2e_fixtures.hpp"
#include "fixture_skip.hpp"

using me::tests::compose::EngineHandle;
using me::tests::compose::TimelineHandle;
using me::tests::compose::JobHandle;
using me::tests::compose::two_track_timeline;

/* --- Video + audio mixer --- */
TEST_CASE("ComposeSink e2e: video + audio track (mixer path) renders") {
    /* Pin the AudioMixer wire-in: a 2-track timeline with 1 video
     * track + 1 audio track both referencing the with-audio fixture.
     * The mixer path activates (has_audio_tracks=true), feeding the
     * AAC encoder directly from mixer output instead of the legacy
     * empty-audio-stream behavior.
     *
     * Can't pixel/sample-compare deterministically (videotoolbox
     * non-deterministic), so this test proves:
     *   - route through compose doesn't crash when an audio track
     *     is declared alongside video
     *   - render completes with ME_OK
     *   - output MP4 exists and is non-empty (>4KB min threshold)
     * Actual sample correctness of mixer output is pinned by
     * test_audio_mixer (silence in → silence out). */
#ifndef ME_TEST_FIXTURE_MP4_WITH_AUDIO
#define ME_TEST_FIXTURE_MP4_WITH_AUDIO ""
#endif
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string fixture_uri = "file://" + fixture_path;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")" + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_vid","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c_aud","assetId":"a1","gainDb":{"static":0},
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "video_plus_audio_mixer.mp4";
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "h264";
    spec.audio_codec = "aac";

    JobHandle job;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job.p) == ME_OK);
    const me_status_t wait_s = me_render_wait(job.p);

    const char* err_msg = me_engine_last_error(eng.p);
    const std::string err_str = err_msg ? std::string{err_msg} : std::string{};
    MESSAGE("video+audio-mixer e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        return;  /* videotoolbox unavailable */
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}

/* --- Loader: transform on audio rejected --- */
TEST_CASE("Loader: transform on audio clip still rejected") {
    /* Negative control for transform-on-text-subtitle-clips: the
     * gate relaxed from "Video only" to "not Audio". Audio clips
     * must still be rejected — 2D positional transform is
     * semantically nonsense for audio. */
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":"file:///tmp/nope.mp4"}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c_a","assetId":"a1",
           "transform":{"opacity":{"static":0.5}},
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    const me_status_t s = me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p);
    CHECK(s == ME_E_PARSE);
    CHECK(tl.p == nullptr);
}

/* --- AudioOnlySink (no video track) --- */
TEST_CASE("AudioOnlySink e2e: audio-only timeline (no video track) renders") {
    /* Pin the audio-only routing path: timeline with only audio
     * tracks routes through make_audio_only_sink instead of the
     * compose factory (which needs a video track). Expected output:
     * valid MP4 with a single AAC audio stream, no video stream. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string fixture_uri = "file://" + fixture_path;
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")" + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"a0","kind":"audio","clips":[
          {"type":"audio","id":"c_aud","assetId":"a1","gainDb":{"static":0},
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "audio_only.mp4";
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "";          /* unused in audio-only path */
    spec.audio_codec = "aac";

    JobHandle job;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job.p) == ME_OK);
    const me_status_t wait_s = me_render_wait(job.p);

    const char* err_msg = me_engine_last_error(eng.p);
    const std::string err_str = err_msg ? std::string{err_msg} : std::string{};
    MESSAGE("audio-only e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        return;
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    /* Audio-only MP4 of silent AAC compresses very small (~1KB for
     * 1s silence + container overhead). Loose bounds. */
    CHECK(fs::file_size(out_path) > 256);        /* must contain audio bytes */
    CHECK(fs::file_size(out_path) < 300000);     /* no video stream smuggled in */
}
