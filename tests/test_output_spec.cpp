/*
 * test_output_spec — tripwire for me_output_spec_t.container inference.
 *
 * Surface asserted:
 *   - Null-/empty-container + known file extension (".mp4", ".mov") →
 *     me_render_start ME_OK, worker completes ME_OK, output file is
 *     produced. Libav infers the muxer from the path's extension.
 *   - Null-container + unrecognised extension (".xyz") → wait returns
 *     ME_E_UNSUPPORTED (not ME_E_INTERNAL — the upstream mapping was
 *     changed in this cycle so host code can branch on a recoverable
 *     status, not an internal-invariant one). Engine last-error string
 *     contains "container format not recognised".
 *   - No output file is left behind on the unsupported path (we don't
 *     strictly require libav to not have touched the path, but in
 *     practice the mux-open failure occurs before any bytes are written).
 *
 * Why this exists: `grep -rn 'output_spec\|container' tests/` before
 * this suite had zero negative-path coverage for container inference.
 * Hosts writing generic "save-to-any-extension" UI would never have
 * discovered this code path via the existing tests.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <cstring>
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

std::string single_clip_timeline(const std::string& fixture_path) {
    namespace tb = me::tests::tb;
    tb::TimelineBuilder b;
    b.frame_rate(25, 1).resolution(640, 480);
    b.add_asset(tb::AssetSpec{.uri = "file://" + fixture_path});
    b.add_clip(tb::ClipSpec{
        .clip_id = "c1",
        .time_start_num = 0, .time_start_den = 25,
        .time_dur_num   = 25, .time_dur_den   = 25,
        .source_start_den = 25,
        .source_dur_num = 25, .source_dur_den = 25,
    });
    return b.build();
}

/* Runs a one-clip passthrough render with container=NULL. Returns the
 * terminal status from me_render_wait. Caller inspects last-error after. */
me_status_t run_with_null_container(me_engine_t* eng, me_timeline_t* tl,
                                     const fs::path& out_path) {
    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = nullptr;          /* rely on extension inference */
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    me_render_job_t* job = nullptr;
    const me_status_t start_s = me_render_start(eng, tl, &spec, nullptr, nullptr, &job);
    if (start_s != ME_OK) return start_s;
    JobHandle jh{job};
    return me_render_wait(jh.p);
}

}  // namespace

TEST_CASE("me_render_start: null-arg validation rejects with ME_E_INVALID_ARG") {
    /* `src/api/render.cpp:40` validates engine / timeline / output /
     * out_job pointers up front and returns ME_E_INVALID_ARG without
     * touching the orchestrator. Pin that contract — a regression
     * that crashes on null engine (deref into thread-local error
     * slot) or that returns ME_OK with a stale out_job would slip
     * through silently. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    TimelineHandle tl;
    const std::string js = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, js.data(), js.size(), &tl.p) == ME_OK);

    me_output_spec_t spec{};
    spec.path        = "/tmp/me_render_start_arg_validation.mp4";
    spec.container   = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    me_render_job_t* job = nullptr;

    SUBCASE("null engine") {
        const me_status_t s = me_render_start(nullptr, tl.p, &spec, nullptr, nullptr, &job);
        CHECK(s == ME_E_INVALID_ARG);
        CHECK(job == nullptr);
    }
    SUBCASE("null timeline") {
        const me_status_t s = me_render_start(eng.p, nullptr, &spec, nullptr, nullptr, &job);
        CHECK(s == ME_E_INVALID_ARG);
        CHECK(job == nullptr);
    }
    SUBCASE("null output spec") {
        const me_status_t s = me_render_start(eng.p, tl.p, nullptr, nullptr, nullptr, &job);
        CHECK(s == ME_E_INVALID_ARG);
        CHECK(job == nullptr);
    }
    SUBCASE("null out_job") {
        const me_status_t s = me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, nullptr);
        CHECK(s == ME_E_INVALID_ARG);
    }
}

TEST_CASE("me_render_start: spec.{width,height,frame_rate.den}=0 inherits from timeline") {
    /* render.h:39-43 documents: "If width/height == 0 or
     * frame_rate.den == 0, inherit from timeline." Pin the inherit
     * path — a regression that propagated 0 to the encoder (dividing
     * by frame_rate.den or sizing a 0x0 output) would crash inside
     * the worker. The existing single-clip passthrough test
     * implicitly covers this since run_with_null_container leaves
     * width=height=0 + frame_rate={0,0}; this case asserts the
     * documented "inherit" behavior explicitly so a future
     * regression that adds a "spec.width must be > 0" guard breaks
     * the test instead of host code. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping: fixture not available");
        return;
    }
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string js = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, js.data(), js.size(), &tl.p) == ME_OK);

    const fs::path out_path = fs::temp_directory_path() /
                               "me_render_spec_inherit.mp4";
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path           = out_path.c_str();
    spec.container      = "mp4";
    spec.video_codec    = "passthrough";
    spec.audio_codec    = "passthrough";
    spec.width          = 0;
    spec.height         = 0;
    spec.frame_rate.num = 0;
    spec.frame_rate.den = 0;

    me_render_job_t* job = nullptr;
    const me_status_t start_s = me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job);
    REQUIRE(start_s == ME_OK);
    JobHandle jh{job};
    const me_status_t wait_s = me_render_wait(jh.p);
    CHECK(wait_s == ME_OK);
    CHECK(fs::exists(out_path));
    fs::remove(out_path);
}

TEST_CASE("me_output_spec.container=NULL + '.mp4' path: libav infers mp4 muxer") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping container-infer test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-output-spec";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "infer.mp4";
    fs::remove(out_path);

    CHECK(run_with_null_container(eng.p, tl.p, out_path) == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 0);
}

TEST_CASE("me_output_spec.container=NULL + '.mov' path: libav infers mov muxer") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-output-spec";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "infer.mov";
    fs::remove(out_path);

    CHECK(run_with_null_container(eng.p, tl.p, out_path) == ME_OK);
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 0);
}

TEST_CASE("me_output_spec.container=NULL + unknown '.xyz' extension: ME_E_UNSUPPORTED") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping unsupported-extension test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-output-spec";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "bad.xyz";
    fs::remove(out_path);

    /* me_render_start itself returns ME_OK — sink construction doesn't
     * look at container; MuxContext::open on the worker thread is where
     * format inference happens. The failure surfaces through
     * me_render_wait + me_engine_last_error. */
    const me_status_t wait_s = run_with_null_container(eng.p, tl.p, out_path);
    CHECK(wait_s == ME_E_UNSUPPORTED);

    const char* le = me_engine_last_error(eng.p);
    REQUIRE(le != nullptr);
    const std::string err{le};
    CHECK(err.find("container format not recognised") != std::string::npos);
    /* The err message echoes the offending path so hosts can surface it
     * in UI without re-plumbing state. */
    CHECK(err.find("bad.xyz") != std::string::npos);
}

TEST_CASE("me_output_spec.container explicitly set to unknown name: ME_E_UNSUPPORTED") {
    /* Complements the extension-inference path: an explicit but
     * unknown container name should also fail with UNSUPPORTED, with
     * a message that names the offending container string. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-output-spec";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "named-bad.mp4";
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "totally-not-a-muxer";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    me_render_job_t* job = nullptr;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job) == ME_OK);
    JobHandle jh{job};
    CHECK(me_render_wait(jh.p) == ME_E_UNSUPPORTED);

    const char* le = me_engine_last_error(eng.p);
    REQUIRE(le != nullptr);
    const std::string err{le};
    CHECK(err.find("totally-not-a-muxer") != std::string::npos);
}
