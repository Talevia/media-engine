/*
 * test_render_job_lifecycle — tripwire for me_render_job_destroy corners
 * not covered by the happy-path tests.
 *
 * Surface asserted:
 *   - me_render_job_destroy(NULL) is a safe no-op (can be called twice).
 *   - Start → destroy-without-wait is supported: destroy joins the worker
 *     thread internally (`src/api/render.cpp:80`), producing cleanup
 *     semantics equivalent to wait → destroy but without caller needing
 *     to plumb the wait call.
 *   - Start → cancel → destroy-without-wait is supported: the common
 *     host pattern of "user pressed cancel, UI walks away" must not leak
 *     or crash. Destroy joins the (now rapidly-exiting) worker.
 *   - Standard start → wait → destroy cycle followed by a NULL destroy
 *     is a no-op (not UB), so host shutdown code can destroy + null-out
 *     + re-destroy during generic cleanup passes.
 *
 * Why this exists: `grep -rn 'me_render_job_destroy' tests/` before this
 * suite only hit happy-path wait→destroy patterns inside other tests'
 * RAII handles. A refactor that moved the worker.join() out of destroy
 * into a separate me_render_job_release() would silently break host
 * code that relies on destroy's implicit join (the documented contract
 * per render.h:67).
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

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

std::string three_clip_timeline(const std::string& fixture_path) {
    namespace tb = me::tests::tb;
    tb::TimelineBuilder b;
    b.frame_rate(25, 1).resolution(640, 480);
    b.add_asset(tb::AssetSpec{.uri = "file://" + fixture_path});
    for (int i = 0; i < 3; ++i) {
        b.add_clip(tb::ClipSpec{
            .clip_id = "c" + std::to_string(i + 1),
            .time_start_num = i * 25, .time_start_den = 25,
            .time_dur_num   = 25,     .time_dur_den   = 25,
            .source_start_den = 25,
            .source_dur_num = 25, .source_dur_den = 25,
        });
    }
    return b.build();
}

}  // namespace

TEST_CASE("me_render_job_destroy(NULL) is a no-op (idempotent)") {
    /* Guard: host shutdown code often destroys + nulls out handles in
     * a loop, and safety of re-calling destroy on a null handle keeps
     * that pattern straightforward. */
    me_render_job_destroy(nullptr);
    me_render_job_destroy(nullptr);   /* second call is still a no-op */
    CHECK(true);  /* absence of crash is the assertion */
}

TEST_CASE("start → destroy without wait: destroy joins worker implicitly") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping destroy-no-wait test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-job-lifecycle";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "destroy-no-wait.mp4";
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    me_render_job_t* job = nullptr;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job) == ME_OK);
    REQUIRE(job != nullptr);

    /* Destroy without wait. The worker thread is still running (or just
     * about to start); destroy must internally join it before freeing
     * the wrapper — otherwise std::thread's destructor would terminate
     * the process with joinable()==true. This test's absence of crash
     * IS the contract check. */
    me_render_job_destroy(job);

    /* Passthrough ran to completion during destroy's internal join, so
     * the output file should exist. (If destroy had short-circuited the
     * worker somehow, the file would be partial or absent — we don't
     * assert mux-level structure here; just existence + non-zero size,
     * which pins the "destroy did wait for the worker" invariant.) */
    CHECK(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 0);
}

TEST_CASE("start → cancel → destroy without wait: no crash, prompt cleanup") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping cancel-then-destroy test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = three_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-job-lifecycle";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "cancel-destroy.mp4";
    fs::remove(out_path);

    /* h264/aac reencode so the worker has substantial work to interrupt.
     * On hosts without videotoolbox the start itself may fail fast —
     * in that case the destroy path is still exercised but on an
     * early-exited worker (also a valid code path to not crash on). */
    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "h264";
    spec.audio_codec = "aac";

    me_render_job_t* job = nullptr;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job) == ME_OK);
    REQUIRE(job != nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(me_render_cancel(job) == ME_OK);

    /* Host's "user pressed cancel, I'm done" pattern: destroy without
     * waiting for wait(). Must not crash (worker.join() inside destroy
     * unblocks quickly because the cancel flag makes the worker exit
     * early). */
    const auto t0 = std::chrono::steady_clock::now();
    me_render_job_destroy(job);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    /* Generous upper bound — reencode might take up to a few seconds to
     * unwind on slower hosts but should never take 30s+ if cancel worked.
     * Value here is CI-safe, not a perf assertion. */
    CHECK(elapsed.count() < 30000);
}

TEST_CASE("start → wait → destroy → destroy(NULL): standard cycle + shutdown-pass safe") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) {
        MESSAGE("skipping standard-cycle test: fixture not available");
        return;
    }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = single_clip_timeline(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path out_path =
        fs::temp_directory_path() / "me-job-lifecycle" / "standard.mp4";
    fs::create_directories(out_path.parent_path());
    fs::remove(out_path);

    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "passthrough";
    spec.audio_codec = "passthrough";

    me_render_job_t* job = nullptr;
    REQUIRE(me_render_start(eng.p, tl.p, &spec, nullptr, nullptr, &job) == ME_OK);
    CHECK(me_render_wait(job) == ME_OK);
    me_render_job_destroy(job);
    job = nullptr;

    /* Host shutdown pass: clean up everything, including handles that
     * may have already been destroyed. destroy(NULL) must be a no-op. */
    me_render_job_destroy(job);  /* NULL-safe per contract */
    CHECK(fs::exists(out_path));
}
