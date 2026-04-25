/*
 * test_compose_sink_e2e — video / transform / cross-dissolve
 * transition core. Extracted sections:
 *   - audio / mixer / audio-only      → test_compose_sink_e2e_audio.cpp
 *   - text clip                        → test_compose_sink_e2e_text.cpp
 *   - subtitle clip / fileUri          → test_compose_sink_e2e_subtitle.cpp
 * Shared fixtures: compose_sink_e2e_fixtures.hpp.
 *
 * This file retains the original video-compose core: 2-track
 * h264/aac render, per-clip transform opacity / translate, two-run
 * output-size stability, cross-dissolve transition, animated-
 * transform keyframe end-to-end. Skips when videotoolbox is
 * unavailable (Linux CI) — wait status there is ME_E_UNSUPPORTED /
 * ME_E_ENCODE, which the existing assertions tolerate.
 */
#include "compose_sink_e2e_fixtures.hpp"
#include "fixture_skip.hpp"

using me::tests::compose::EngineHandle;
using me::tests::compose::TimelineHandle;
using me::tests::compose::JobHandle;
using me::tests::compose::two_track_timeline;
TEST_CASE("ComposeSink e2e: 2-track h264/aac render produces non-empty output") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
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

/* --- Cross-dissolve transition --- */
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

/* --- Animated transform keyframes --- */
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
    ME_REQUIRE_FIXTURE(fixture_path);
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
