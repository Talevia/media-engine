/*
 * test_render_cancel — cooperative cancellation tripwire for me_render_cancel.
 *
 * Surface asserted:
 *   - me_render_cancel flips Job::cancel atomic → worker sees it inside
 *     its read-frame loop and returns ME_E_CANCELLED from reencode_mux
 *     / passthrough_mux.
 *   - me_render_wait returns ME_E_CANCELLED when the render exited via
 *     the cancel path (not the COMPLETED one).
 *   - Progress callback sees FAILED kind with status == ME_E_CANCELLED
 *     (not COMPLETED).
 *   - me_render_job_destroy is safe to call after a cancelled render.
 *
 * No CI tripwire before this suite: `grep -rn 'me_render_cancel\|cancel' tests/`
 * returned empty, so any refactor of the cancel propagation path
 * (cooperative-check frequency, race on double-cancel, post-cancel
 * callback delivery) could silently regress.
 *
 * Uses the shared determinism fixture — the two-clip passthrough concat
 * takes long enough for a `sleep 50ms → cancel` pattern to catch the
 * worker mid-loop on every host we care about.
 */
#include <doctest/doctest.h>

#include <media_engine.h>

#include "timeline_builder.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "fixture_skip.hpp"

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

/* Thread-safe event capture for the progress callback. doctest assertions
 * are not thread-safe, so the callback just appends + signals; the main
 * test thread waits and then inspects. */
struct ProgressLog {
    std::mutex                            mtx;
    std::vector<me_progress_event_t>      events;
    std::atomic<bool>                     terminal_seen{false};
};

void on_progress(const me_progress_event_t* ev, void* user) {
    auto* log = static_cast<ProgressLog*>(user);
    /* Deep-copy the event — `message` / `output_path` are transient
     * per callback invocation; we don't inspect them in this suite so
     * zeroing them out is fine. */
    me_progress_event_t snap = *ev;
    snap.message     = nullptr;
    snap.output_path = nullptr;
    {
        std::lock_guard<std::mutex> lk(log->mtx);
        log->events.push_back(snap);
    }
    if (ev->kind == ME_PROGRESS_COMPLETED || ev->kind == ME_PROGRESS_FAILED) {
        log->terminal_seen.store(true, std::memory_order_release);
    }
}

std::string long_timeline_json(const std::string& fixture_path) {
    /* 3 clips × 1s each → 3s target render. Passthrough is stream-copy
     * (finishes in tens of ms on this fixture), so the cancel test
     * uses h264/aac reencode instead — that goes through libavcodec
     * decode + VideoToolbox encode + AAC, several hundred ms per
     * segment on dev hardware. 3 segments is enough that `sleep(200ms)
     * → cancel` reliably lands inside `process_segment`'s read-loop. */
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

/* Helper: attempt to start a render through the reencode path; skip the
 * whole test on hosts without h264_videotoolbox (Linux CI). */
me_status_t start_reencode_job(me_engine_t* eng, me_timeline_t* tl,
                                const fs::path& out_path,
                                me_progress_cb cb, void* user,
                                me_render_job_t** out_job) {
    me_output_spec_t spec{};
    spec.path        = out_path.c_str();
    spec.container   = "mp4";
    spec.video_codec = "h264";
    spec.audio_codec = "aac";
    return me_render_start(eng, tl, &spec, cb, user, out_job);
}

}  // namespace

TEST_CASE("me_render_cancel mid-render: wait returns ME_E_CANCELLED") {
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    ME_REQUIRE_FIXTURE(fixture_path);
    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);

    TimelineHandle tl;
    const std::string j = long_timeline_json(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path tmp_dir = fs::temp_directory_path() / "me-cancel-test";
    fs::create_directories(tmp_dir);
    const fs::path out_path = tmp_dir / "cancel.mp4";
    fs::remove(out_path);

    ProgressLog log;
    JobHandle job;
    REQUIRE(start_reencode_job(eng.p, tl.p, out_path, on_progress, &log, &job.p) == ME_OK);

    /* Wait for the reencode worker to enter process_segment's read-
     * loop, then cancel. h264/aac reencode on 3s of 640×480 25fps
     * takes well over 200ms on any dev / CI host — enough window to
     * land the cancel mid-render reliably. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(me_render_cancel(job.p) == ME_OK);

    /* Wait returns ME_E_CANCELLED (not ME_OK) when cancel won the race.
     * When videotoolbox is unavailable (Linux CI), the reencode start
     * itself already returned ME_E_UNSUPPORTED / ME_E_ENCODE through
     * the progress callback's FAILED event; wait just reports that
     * terminal status. Skip the strict cancel assertion in that case
     * — the cancel path isn't what we tested if the encoder never
     * opened. */
    const me_status_t wait_s = me_render_wait(job.p);
    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        MESSAGE("skipping cancel assertion: h264_videotoolbox unavailable (status="
                << me_status_str(wait_s) << ")");
        return;
    }
    CHECK(wait_s == ME_E_CANCELLED);

    /* Progress callback must have seen FAILED with ME_E_CANCELLED
     * (and NOT COMPLETED) — inverse of the happy path. */
    bool saw_started   = false;
    bool saw_completed = false;
    bool saw_cancelled_failure = false;
    {
        std::lock_guard<std::mutex> lk(log.mtx);
        for (const auto& ev : log.events) {
            if (ev.kind == ME_PROGRESS_STARTED)   saw_started   = true;
            if (ev.kind == ME_PROGRESS_COMPLETED) saw_completed = true;
            if (ev.kind == ME_PROGRESS_FAILED && ev.status == ME_E_CANCELLED) {
                saw_cancelled_failure = true;
            }
        }
    }
    CHECK(saw_started);
    CHECK_FALSE(saw_completed);
    CHECK(saw_cancelled_failure);
}

TEST_CASE("me_render_cancel is idempotent — double-call returns ME_OK") {
    /* Defensive: host code often hits cancel twice (user double-clicks
     * cancel button, or a cleanup path retries). Contract is that
     * re-cancelling a cancelling or cancelled job is a no-op, not an
     * error — callers shouldn't need bookkeeping to avoid the second
     * call. */
    const std::string fixture_path = ME_TEST_FIXTURE_MP4;
    if (fixture_path.empty() || !fs::exists(fixture_path)) { return; }

    EngineHandle eng;
    REQUIRE(me_engine_create(nullptr, &eng.p) == ME_OK);
    TimelineHandle tl;
    const std::string j = long_timeline_json(fixture_path);
    REQUIRE(me_timeline_load_json(eng.p, j.data(), j.size(), &tl.p) == ME_OK);

    const fs::path out_path = fs::temp_directory_path() / "me-cancel-double.mp4";
    fs::remove(out_path);

    JobHandle job;
    REQUIRE(start_reencode_job(eng.p, tl.p, out_path, nullptr, nullptr, &job.p) == ME_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(me_render_cancel(job.p) == ME_OK);
    CHECK(me_render_cancel(job.p) == ME_OK);   /* second cancel still ME_OK */

    const me_status_t wait_s = me_render_wait(job.p);
    if (wait_s == ME_E_UNSUPPORTED || wait_s == ME_E_ENCODE) {
        MESSAGE("skipping double-cancel assertion: h264_videotoolbox unavailable");
        return;
    }
    CHECK(wait_s == ME_E_CANCELLED);
}

TEST_CASE("me_render_cancel(NULL) returns ME_E_INVALID_ARG") {
    /* Guard: cancel must not deref a null handle. */
    CHECK(me_render_cancel(nullptr) == ME_E_INVALID_ARG);
}
