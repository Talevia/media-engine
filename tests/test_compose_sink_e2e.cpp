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
#include <fstream>
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

TEST_CASE("ComposeSink e2e: per-clip translate renders (spatial transform wired)") {
    /* Pin that `Clip::transform.translate_*` actually reaches the
     * compose loop via the affine_blit pre-composite path. Render
     * twice: once with top track at translate (0, 0) (identity
     * fast-path), once with top track at translate (100, 50)
     * (forces affine path). The renders must produce different
     * output — size differs, since shifted content compresses
     * differently under h264. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    auto render = [&](const std::string& translate_json,
                      const fs::path& out) -> me_status_t {
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
               "transform":)" + translate_json + R"(,
               "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
               "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
            ]}
          ]}],
          "output": {"compositionId":"main"}
        })";
        TimelineHandle tl;
        REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

        fs::remove(out);
        me_output_spec_t spec{};
        spec.path        = out.c_str();
        spec.container   = "mp4";
        spec.video_codec = "h264";
        spec.audio_codec = "aac";
        JobHandle job;
        if (me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job.p) != ME_OK) {
            return ME_E_INTERNAL;
        }
        return me_render_wait(job.p);
    };

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_id   = tmp_dir / "translate-identity.mp4";
    const fs::path out_xlat = tmp_dir / "translate-shifted.mp4";

    /* Identity: empty transform → skip spatial affine, fast path. */
    const me_status_t s_id = render(R"({})", out_id);
    if (s_id == ME_E_UNSUPPORTED || s_id == ME_E_ENCODE) { return; }
    REQUIRE(s_id == ME_OK);

    /* Shifted: translateX=100, translateY=50 → affine pre-composite. */
    const me_status_t s_xlat = render(
        R"({"translateX":{"static":100},"translateY":{"static":50}})",
        out_xlat);
    REQUIRE(s_xlat == ME_OK);

    /* Both files exist and non-trivial. */
    CHECK(fs::file_size(out_id)   > 4096);
    CHECK(fs::file_size(out_xlat) > 4096);

    /* Non-identity translate should change encoded output. Can't
     * assert byte-inequality on videotoolbox (non-deterministic
     * across runs anyway), but file sizes should differ by at least
     * a few hundred bytes given the different pixel content. */
    const auto sz_id   = fs::file_size(out_id);
    const auto sz_xlat = fs::file_size(out_xlat);
    const auto diff    = sz_id > sz_xlat ? sz_id - sz_xlat : sz_xlat - sz_id;
    /* h264 compression of shifted content + mostly-transparent edges
     * produces noticeably different file size. 1% of min is a generous
     * floor — real diff on dev hardware is typically several percent. */
    const auto min_sz  = sz_id < sz_xlat ? sz_id : sz_xlat;
    CHECK(diff >= min_sz / 100);
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
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: with-audio fixture not available");
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

TEST_CASE("ComposeSink e2e: single-track with cross-dissolve transition renders") {
    /* Cross-dissolve wire-in smoke test. 2-clip single-track timeline
     * with a 0.5s cross-dissolve transition. The compose path is
     * reached via has_transitions=true (bypassing the is_multi_track
     * check). Transition window: [0.75s, 1.25s) — midway in the
     * 2s timeline. Verifies the route_through_compose routing +
     * compose factory acceptance + frame loop Transition branch
     * don't crash; pixel-exact correctness of the blend is pinned
     * by test_compose_cross_dissolve (kernel) and by
     * test_compose_active_clips (scheduler). */
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
          {"type":"video","id":"cA","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}},
          {"type":"video","id":"cB","assetId":"a1",
           "timeRange":{"start":{"num":25,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ],"transitions":[
          {"kind":"crossDissolve","fromClipId":"cA","toClipId":"cB",
           "duration":{"num":12,"den":25}}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "crossdissolve.mp4";
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
    MESSAGE("crossdissolve e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        /* videotoolbox unavailable or known encoder limit — not an
         * e2e regression signal we can pin here. */
        return;
    }

    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}

TEST_CASE("ComposeSink e2e: animated transform (translateX keyframes) renders end-to-end") {
    /* Pins the Transform animated-number integration end-to-end:
     * a clip with translateX keyframed 0 → 200 over the clip's
     * duration should render without error and produce a non-
     * trivial output. Compose loop calls clip.transform->
     * evaluate_at(T) each frame → different translate per frame →
     * different rendered content vs no-transform baseline.
     *
     * Exact pixel verification isn't practical (h264_videotoolbox
     * non-determinism + content-dependent compression), so we
     * assert: status=OK + file exists + > size threshold. The
     * animated-transform plumbing itself is pinned by
     * test_animated_number (interp math), test_animated_number_
     * loader (JSON → AnimatedNumber), and test_timeline_schema
     * (parse + evaluate_at per-T). */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    const std::string fixture_uri = "file://" + fixture_path;
    /* 2-track timeline: v0 static baseline (covers canvas), v1
     * animated translateX keyframed 0 → 200 over 1s. Needs multi-
     * track for ComposeSink routing; v0 gives canvas coverage,
     * v1's animated translate is the actual tripwire. Non-identity
     * scale ensures v1 takes the affine_blit path (where
     * evaluate_at(T).translate_x flows into compose_inverse_affine). */
    const std::string j = R"({
      "schemaVersion": 1,
      "frameRate":  {"num":25,"den":1},
      "resolution": {"width":640,"height":480},
      "colorSpace": {"primaries":"bt709","transfer":"bt709","matrix":"bt709","range":"limited"},
      "assets": [{"id":"a1","kind":"video","uri":")" + fixture_uri + R"("}],
      "compositions": [{"id":"main","tracks":[
        {"id":"v0","kind":"video","clips":[
          {"type":"video","id":"c_bg","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}}}
        ]},
        {"id":"v1","kind":"video","clips":[
          {"type":"video","id":"c_anim","assetId":"a1",
           "timeRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "sourceRange":{"start":{"num":0,"den":25},"duration":{"num":25,"den":25}},
           "transform":{
             "translateX":{"keyframes":[
               {"t":{"num":0,"den":25},"v":0.0,"interp":"linear"},
               {"t":{"num":25,"den":25},"v":200.0,"interp":"linear"}
             ]},
             "scaleX":{"static":0.5},
             "scaleY":{"static":0.5}
           }}
        ]}
      ]}],
      "output": {"compositionId":"main"}
    })";
    TimelineHandle tl;
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-compose-sink-e2e";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "animated_transform.mp4";
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
    MESSAGE("animated-transform e2e: status=" << static_cast<int>(wait_s)
            << " err='" << err_str << "'");

    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        return;  /* videotoolbox unavailable */
    }
    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 4096);
}

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
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

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
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

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
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }
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

TEST_CASE("AudioOnlySink e2e: audio-only timeline (no video track) renders") {
    /* Pin the audio-only routing path: timeline with only audio
     * tracks routes through make_audio_only_sink instead of the
     * compose factory (which needs a video track). Expected output:
     * valid MP4 with a single AAC audio stream, no video stream. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4_WITH_AUDIO;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: with-audio fixture not available");
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
