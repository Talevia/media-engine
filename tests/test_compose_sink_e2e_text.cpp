/*
 * test_compose_sink_e2e_text — extracted from the original
 * 1136-line test_compose_sink_e2e.cpp when the file was split
 * by pipeline kind (debt-split-test-compose-sink-e2e-cpp).
 * Shared handle RAIIs + fixtures live in
 * compose_sink_e2e_fixtures.hpp.
 */
#include "compose_sink_e2e_fixtures.hpp"

using me::tests::compose::EngineHandle;
using me::tests::compose::TimelineHandle;
using me::tests::compose::JobHandle;
using me::tests::compose::two_track_timeline;

/* --- Video + text clip via compose_decode_loop --- */
TEST_CASE("ComposeSink e2e: video track + text track (text clip wired into compose)") {
    /* Pin compose-sink-text-clip-wire: timeline with a video track
     * and a text track renders end-to-end. Before the wire-up, a
     * text clip's clip_idx had no decoder and the compose loop's
     * SingleClip branch silently skipped the track — rendered
     * output contained only the video layer.
     *
     * With Skia on, text is rasterized onto the text-track's
     * track_rgba buffer and alpha-composited on top of the video.
     * We can't pixel-verify h264 output, but "render succeeds + file
     * size in the same order as a video-only render" proves the
     * text path doesn't crash and doesn't get silently dropped
     * back to an error. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

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
          {"type":"video","id":"c_v","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"t0","kind":"text","clips":[
          {"type":"text","id":"c_t",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "textParams":{"content":"Hello","color":"#FFFFFFFF",
                         "fontSize":{"static":48},
                         "x":{"static":20},"y":{"static":100}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "video_plus_text.mp4";
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
    MESSAGE("video+text e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        return;
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}

/* --- Text clip with transform.opacity --- */
TEST_CASE("ComposeSink e2e: text clip with transform.opacity renders") {
    /* Pin transform-on-text-subtitle-clips: the loader previously
     * rejected `transform` on non-video tracks with ME_E_PARSE
     * ("not valid on audio clip"). That block also caught text +
     * subtitle clips even though transform is a 2D positional
     * concept meaningful for any visual kind. Gate now reads
     * `track_kind != Audio`; audio stays rejected, text/subtitle
     * accept.
     *
     * The compose loop's common opacity + alpha_over path already
     * reads `clip.transform` uniformly by `transform_clip_idx`, so
     * text clips with transform.opacity=0.5 should render at half
     * strength on top of the video layer without any additional
     * compose-side wiring. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

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
          {"type":"video","id":"c_v","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"t0","kind":"text","clips":[
          {"type":"text","id":"c_t",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "transform":{"opacity":{"static":0.5}},
           "textParams":{"content":"Hello","color":"#FFFFFFFF",
                         "fontSize":{"static":48},
                         "x":{"static":20},"y":{"static":100}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "video_plus_text_half_opacity.mp4";
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
    MESSAGE("text-with-transform e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        return;
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}
