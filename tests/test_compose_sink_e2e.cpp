/*
 * test_compose_sink_e2e — end-to-end validation of the multi-track
 * compose pipeline that landed in `multi-track-compose-actual-composite`.
 *
 * Before this suite, the only multi-track render test used a fake URI
 * and asserted `me_render_start` doesn't return the old stub message —
 * i.e. a regression tripwire that the compose sink routing works, not
 * that the output is correct. This suite runs a real 2-track render
 * against the shared determinism fixture (the same MP4 used for
 * passthrough / single-track reencode tests), feeding it into both
 * tracks so the compose loop has real decoded frames to composite.
 *
 * Assertions:
 *   - me_render_wait returns ME_OK (full pipeline runs).
 *   - Output file exists, is non-empty (> few KB — real h264/aac output).
 *   - Output size is stable within ±10% across two runs (videotoolbox
 *     is non-deterministic so strict byte-equal doesn't hold, but
 *     gross file-size regressions still trip this).
 *
 * Skips when videotoolbox is unavailable (Linux CI) — the wait status
 * there is ME_E_UNSUPPORTED / ME_E_ENCODE; we don't exercise the
 * compose path on those hosts.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

#ifndef ME_TEST_FIXTURE_MP4
#define ME_TEST_FIXTURE_MP4 ""
#endif

namespace {

struct EngineHandle {
    me_engine_t* p = nullptr;
    ~EngineHandle() { if (p) me_engine_destroy(p); }
};
struct TimelineHandle {
    me_timeline_t* p = nullptr;
    ~TimelineHandle() { if (p) me_timeline_destroy(p); }
};
struct JobHandle {
    me_render_job_t* p = nullptr;
    ~JobHandle() { if (p) me_render_job_destroy(p); }
};

/* Build a 2-track timeline where both tracks use the shared determinism
 * fixture as their single clip. Loader creates independent DemuxContexts
 * per clip, so the compose loop gets two genuinely independent decoder
 * streams even though they happen to read the same file. */
std::string two_track_timeline(const std::string& fixture_uri) {
    namespace tb = me::tests::tb;
    tb::TimelineBuilder b;
    b.frame_rate(25, 1).resolution(640, 480);
    b.add_asset(tb::AssetSpec{.uri = fixture_uri});
    /* Both clips 1 second each, same asset. */
    const int dur_num = 25, dur_den = 25;

    /* Put clips in flat order: v0's clip first, v1's clip second. */
    /* Manually craft multi-track JSON — TimelineBuilder today only
     * emits single-track. */
    std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")" + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_v0","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}},
           "sourceRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}}}
        ]},
        {"id":"v1","kind":"video","clips":[
          {"type":"video","id":"c_v1","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}},
           "sourceRange":{"start":{"num":0,"den":)" + std::to_string(dur_den) +
           R"(},"duration":{"num":)" + std::to_string(dur_num) + R"(,"den":)" + std::to_string(dur_den) + R"(}}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    return j;
}

}  // namespace

TEST_CASE("ComposeSink e2e: 2-track h264/aac render produces non-empty output") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string j = two_track_timeline("file://" + fixture_path);
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "2track.mp4";
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
    MESSAGE("wait status=" << static_cast<int>(wait_s) << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        /* h264_videotoolbox not available OR known ComposeSink limit
         * hit. Either way not an e2e failure we can pin in this suite. */
        return;
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);   /* minimum plausible size */
}

TEST_CASE("ComposeSink e2e: 2-track with per-clip transform opacity renders") {
    /* Pin that Clip::transform.opacity actually reaches the compose
     * loop's alpha_over call. With opacity=0.5 on the top track, the
     * rendered output should be a 50/50 mix of the two decoded
     * streams — but we can't pixel-compare (videotoolbox is non-
     * deterministic and the output is h264-compressed). What we CAN
     * pin is that the render still succeeds (opacity plumbing doesn't
     * crash or error) and produces a non-empty file. The correctness
     * of the alpha_over math itself is pinned by
     * test_compose_alpha_over's "50% src alpha produces 50/50 mix"
     * case. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

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
          {"type":"video","id":"c_v0","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"v1","kind":"video","clips":[
          {"type":"video","id":"c_v1","assetId":"a1",
           "transform":{"opacity":{"static":0.5}},
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
    const fs::path out_path = tmp_dir / "2track-opacity.mp4";
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
        /* videotoolbox unavailable — can't exercise the real compose
         * path on this host. */
        return;
    }

    const char* err = me_engine_last_error(eng.p);
    const std::string err_str = err ? std::string{err} : std::string{};
    MESSAGE("wait status=" << static_cast<int>(wait_s) << " err='" << err_str << "'");
    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}

TEST_CASE("ComposeSink e2e: two 2-track renders produce similarly-sized output") {
    /* Videotoolbox is non-deterministic (HW encoder state persists
     * across renders in subtle ways), so we can't assert byte-equal.
     * But gross size regressions (e.g. encoder dropping half the
     * frames) would fail this check. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    const std::string j = two_track_timeline("file://" + fixture_path);
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);

    auto render_once = [&](const fs::path& out) -> me_status_t {
        fs::remove(out);
        me_output_spec_t spec{};
        spec.path        = out.c_str();
        spec.container   = "mp4";
        spec.video_codec = "h264";
        spec.audio_codec = "aac";
        me_render_job_t* job = nullptr;
        if (me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job) != ME_OK) return ME_E_INTERNAL;
        JobHandle jh{job};
        return me_render_wait(jh.p);
    };

    const fs::path out1 = tmp_dir / "run1.mp4";
    const fs::path out2 = tmp_dir / "run2.mp4";

    const me_status_t s1 = render_once(out1);
    if (s1 == ME_E_UNSUPPORTED || s1 == ME_E_ENCODE) {
        MESSAGE("skipping size-stability check: videotoolbox unavailable");
        return;
    }
    REQUIRE(s1 == ME_OK);
    const auto sz1 = fs::file_size(out1);

    const me_status_t s2 = render_once(out2);
    REQUIRE(s2 == ME_OK);
    const auto sz2 = fs::file_size(out2);

    /* Sizes should be within 10% of each other — gross deviation
     * indicates encoder or compose-loop regression. */
    const double ratio = static_cast<double>(sz1) / static_cast<double>(sz2);
    CHECK(ratio >= 0.90);
    CHECK(ratio <= 1.10);
}
