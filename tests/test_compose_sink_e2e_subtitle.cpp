/*
 * test_compose_sink_e2e_subtitle — extracted from the original
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

TEST_CASE("ComposeSink e2e: video track + subtitle track (subtitle clip wired into compose)") {
    /* Pin compose-sink-subtitle-track-wire: timeline with a video
     * track + a subtitle track renders end-to-end. Before this
     * cycle, TrackKind::Subtitle didn't exist — the loader rejected
     * "subtitle" as an unknown track kind. Now we accept the track,
     * parse an inline .srt content string into SubtitleClipParams,
     * and wire compose_decode_loop's subtitle branch through
     * SubtitleRenderer (libass).
     *
     * Inline .srt (1 cue covering the clip's full duration) keeps
     * the test self-contained. Render just has to succeed + produce
     * a plausible-sized file — libass actually drawing the glyphs
     * is covered by test_subtitle_renderer's unit cases. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string fixture_uri = "file://" + fixture_path;
    /* Inline .srt content, JSON-escaped (\n as "\\n") so the whole
     * timeline is a single valid JSON literal. One cue covering
     * 0 → 1s. */
    const std::string srt_json_escaped =
        "1\\n00:00:00,000 --> 00:00:01,000\\nHello subs\\n\\n";
    const std::string j_final = std::string(R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")") + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_v","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"s0","kind":"subtitle","clips":[
          {"type":"subtitle","id":"c_s",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "subtitleParams":{"content":")" + srt_json_escaped + R"("}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j_final.data(), j_final.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "video_plus_subtitle.mp4";
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
    MESSAGE("video+subtitle e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        return;
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}

TEST_CASE("ComposeSink e2e: subtitle clip with fileUri (external .srt)") {
    /* Pin subtitle-clip-file-uri: SubtitleClipParams may source
     * its bytes from an inline `content` string OR a `file_uri`
     * pointing at an external .srt / .ass file. Writes a .srt
     * fixture into /tmp, feeds it via fileUri in the timeline
     * JSON, and asserts render succeeds. Matches the
     * compose-sink-subtitle-track-wire test shape but exercises
     * the new filesystem branch of compose_decode_loop's
     * subtitle lazy-init. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path srt_path = tmp_dir / "external.srt";
    {
        std::ofstream out(srt_path);
        out << "1\n00:00:00,000 --> 00:00:01,000\nFile URI subs\n\n";
    }
    REQUIRE(fs::exists(srt_path));

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string fixture_uri = "file://" + fixture_path;
    const std::string srt_uri     = "file://" + srt_path.string();
    const std::string j = std::string(R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")") + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_v","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"s0","kind":"subtitle","clips":[
          {"type":"subtitle","id":"c_s",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "subtitleParams":{"fileUri":")" + srt_uri + R"("}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path out_path = tmp_dir / "video_plus_subtitle_fileuri.mp4";
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
    MESSAGE("subtitle-file-uri e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    fs::remove(srt_path);

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        return;
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}

TEST_CASE("ComposeSink e2e: subtitle fileUri not readable → me_engine_last_error populated") {
    /* Pin subtitle-file-uri-error-diagnosis: compose_decode_loop
     * previously degraded silently when a subtitle fileUri
     * couldn't be opened (valid() stayed false, render was a
     * no-op, last_error empty). Hosts had no signal that the
     * fileUri was wrong. After the fix the subtitle branch
     * returns ME_E_IO via the shared err channel so
     * me_render_wait propagates into me_engine_last_error. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string fixture_uri = "file://" + fixture_path;
    /* Pick a path that will not exist. Using the tmp namespace
     * keeps this portable across macOS / Linux hosts. */
    const fs::path bogus_path = fs::temp_directory_path() /
                                 "me-compose-sink-e2e-bogus-missing.srt";
    fs::remove(bogus_path);
    REQUIRE_FALSE(fs::exists(bogus_path));

    const std::string j = std::string(R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")") + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_v","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"s0","kind":"subtitle","clips":[
          {"type":"subtitle","id":"c_s",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "subtitleParams":{"fileUri":"file://)" + bogus_path.string() + R"("}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "video_plus_subtitle_missing.mp4";
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "h264";
    spec.audio_codec = "aac";

    JobHandle job;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job.p) == ME_OK);
    const me_status_t wait_s = me_render_wait(job.p);

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        /* videotoolbox / aac unavailable — skip without
         * asserting the subtitle-specific error (encoder-level
         * error would overshadow). */
        return;
    }

    CHECK(wait_s == ME_E_IO);
    const char* err_msg = me_engine_last_error(eng.p);
    REQUIRE(err_msg != nullptr);
    const std::string err_str{err_msg};
    CHECK(err_str.find("subtitle") != std::string::npos);
    CHECK(err_str.find("not readable") != std::string::npos);
    CHECK(err_str.find(bogus_path.string()) != std::string::npos);
}

TEST_CASE("Loader: subtitle clip with neither content nor fileUri rejected") {
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{"id":"main","tracks":[
        {"id":"s0","kind":"subtitle","clips":[
          {"type":"subtitle","id":"c_s",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "subtitleParams":{}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    const me_status_t s = me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p);
    CHECK(s == ME_E_PARSE);
    CHECK(tl.p == nullptr);
}

TEST_CASE("Loader: subtitle clip with BOTH content and fileUri rejected") {
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [],
      "compositions": [{"id":"main","tracks":[
        {"id":"s0","kind":"subtitle","clips":[
          {"type":"subtitle","id":"c_s",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "subtitleParams":{"content":"1\\n00:00:00,000 --> 00:00:01,000\\nx\\n\\n",
                              "fileUri":"file:///tmp/nope.srt"}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    const me_status_t s = me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p);
    CHECK(s == ME_E_PARSE);
    CHECK(tl.p == nullptr);
}
